/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor.cppm
 *
 * Heavy minimap/pipe implementation for BatchProcessor. Kept out of the
 * dispatch_buffer interface so edits to tuple-buffer or admin code do not
 * force that translation unit to reinstantiate the full supported alphabet x
 * storage-width minimap pipeline.
 *
 * Read/recompute/persist contract for minimizer indexes
 * -----------------------------------------------------
 *
 * There are two separate policy concepts in this file:
 *
 * 1. The active minimizer policy (`ScannerPgContext::active_index`).
 *    - Resolved once for the target column when a user relation is known.
 *      `bioseq.read_sequence_index` controls whether stored chunks are read;
 *      recompute paths still use this policy to choose transient MPF vs MPS.
 *    - Default when no enabled active row is found is
 *      `minimizers_per_fragment`.
 *    - The SQL schema has a unique partial index on
 *      `(user_relid, main_relfilenode, column_attno) WHERE is_active_read`, and
 *      `bioseq.set_active_read_index()` clears old active rows before setting
 *      the new one. Therefore more than one active read index is not a normal
 *      state. If an old/corrupt catalog ever exposes more than one, the resolver
 *      uses `LIMIT 1` with no ordering; treat that as unsupported.
 *
 * 2. The deferred recompute persistence choice.
 *    - Used only when `can_defer_recompute_minimizer_persist()` is true:
 *      `bioseq.recompute_async_persist = on`,
 *      `bioseq.index_async_persist = on`, a user relation is known, and the
 *      effective minimizer parameters are the persisted-index pair `k=15,w=10`.
 *    - It ignores `active_index` and scans `MINIMIZER_INDICES` in persistence
 *      preference order: `minimizers_per_fragment`, then
 *      `minimizers_per_sequence`.
 *    - If both minimizer indexes are enabled for UPDATE_ON_INSERT,
 *      `minimizers_per_fragment` wins. If neither is enabled, no deferred
 *      minimizer persist target is selected.
 *
 * GUC meanings in this dispatch path:
 *
 * - `bioseq.update_index_on_insert`: write/load-time policy gate. When off,
 *   INSERT/load does not build UPDATE_ON_INSERT indexes. It does not by itself
 *   change the read path except by making stored indexes absent.
 *
 * - `bioseq.read_sequence_index`: read-time gate for stored-index reuse. When
 *   on, each row tries to assemble the active stored index if chunks exist.
 *   When off, no stored minimizer index is read even if chunks are present;
 *   rows go to recompute/transient paths that still honor the active MPF/MPS
 *   policy shape.
 *
 * - `bioseq.index_async_persist`: global async persistence gate. If off, async
 *   staging functions refuse to enqueue/write opportunistically built indexes.
 *   It does not pick an index by itself.
 *
 * - `bioseq.recompute_async_persist`: no-index recompute gate. It allows a
 *   recompute path to consult UPDATE_ON_INSERT minimizer policy and stage or
 *   otherwise handle a deferred minimizer target, but only together with
 *   `bioseq.index_async_persist` and only for persisted parameters `k=15,w=10`.
 *
 * - `bioseq.index_memory_cap_mb`: memory-admission cap for resident targets,
 *   assembled indexes, sorted runs, and opportunistic RAM-first builds. `0`
 *   means use the live process/system memory budget. A larger or smaller cap
 *   can admit or reject resident/parallel paths, but it does not force
 *   persistence and it does not choose the active index.
 *
 * - `bioseq.scheduler.pipeline_concurrency_mode` and
 *   `bioseq.scheduler.max_concurrency`: enable task-parallel segment work when
 *   mode is `task_parallel` and effective concurrency is greater than one.
 *   These affect speed and resident materialization attempts, not the logical
 *   persistence policy.
 *
 * Stored-index present:
 *
 * - Active read index = `minimizers_per_fragment`.
 *   If the row has fragment-index chunks, assemble the required full/segment
 *   index from stored chunks and search with it. No new persistence is staged.
 *   If chunks are missing, fall through to recompute.
 *
 * - Active read index = `minimizers_per_sequence`.
 *   If the row has MPS chunks, assemble a query-specific index from the stored
 *   MPS representation and search with it. Legacy logical-fragment-backed MPS
 *   chunks are also supported. No new persistence is staged. If chunks are
 *   missing, fall through to recompute.
 *
 * - No active read row.
 *   The resolver defaults to `minimizers_per_fragment` when reads are enabled.
 *   If `bioseq.read_sequence_index` is off, no stored index is read; the
 *   resolved policy still chooses the transient recompute shape.
 *
 * Stored-index missing or reads disabled:
 *
 * - Active read index = `minimizers_per_fragment`.
 *   Recompute the search index. If RAM-first opportunistic build is admitted,
 *   the computed fragment chunks may be staged under
 *   `minimizers_per_fragment` when `bioseq.index_async_persist` is on.
 *   In later resident recompute paths, if deferred choice also selects
 *   `minimizers_per_fragment`, the recomputed fragment index is written to a
 *   temp payload and staged for async persistence.
 *
 * - Active read index = `minimizers_per_sequence`.
 *   First, the RAM-first opportunistic branch may build fragment-shaped chunks
 *   and stage them under the MPS name if memory admission succeeds and
 *   `bioseq.index_async_persist` is on. If that is not admitted, the DOC0301
 *   fast no-index MPS branch can run when the deferred recompute gates are on
 *   and the UPDATE_ON_INSERT minimizer choice is MPS. That branch materializes
 *   the target in memory when admitted, builds query-filtered transient segment
 *   indexes, searches, and logs `persisted=false`. This is an intentional
 *   semantic exception: the GUCs are used as eligibility gates, but no durable
 *   MPS rows are staged by that fast path.
 *
 * - No UPDATE_ON_INSERT minimizer choice.
 *   Recompute and search normally. If `bioseq.recompute_async_persist` was on
 *   but no eligible minimizer policy exists, the code logs that async-persist
 *   was skipped.
 *
 *-------------------------------------------------------------------------
 */
HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_ONLY) \
    || defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_ONLY) \
    || defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_ONLY) \
    || defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_ONLY) \
    || defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_ONLY)
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_HELPER_ONLY 1
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_ONLY) \
    || defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_HELPER_ONLY)
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_HELPER_ONLY 1
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_SCANNER_HELPER)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION must name the concrete scanner helper"
#endif
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION(
    compact_sequence_range<compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>>& query_seq,
    const pgseq_t* target_hdr,
    std::size_t i,
    sequence_size_t target_bp_size,
    std::size_t min_bp,
    const Minimap2Params& dispatch_params,
    const Minimap2Params& params,
    bool per_row_query_mode,
    std::optional<std::vector<MinimapSketchData>>& cached_query_minimizers,
    std::optional<pipe::MinimizerRangeExecutionProof>&
        cached_query_minimizer_proof,
    bool& consumed_shared_cached_query_proof,
    uint8_t alphabet_code,
    uint8_t storage_unit_byte_width,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx,
    bioseq::parallelism::GlobalPool& pool);
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_HELPER)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION must name the concrete in-memory helper"
#endif
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    sequence_size_t target_bp_size,
    const Minimap2Params& params,
    bool chunked_scanner_forced);
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_LEAF_HELPERS)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION must name the concrete standard in-memory helper"
#endif
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION must name the concrete segmented in-memory helper"
#endif
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION must name the concrete chunked in-memory helper"
#endif
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION must name the concrete chunked segmented in-memory helper"
#endif
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params);
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params);
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params);
[[nodiscard]] MatchResultCollection BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params);
#endif

namespace {

constexpr std::string_view kMinimizersPerFragmentIndex =
    bioseq::indexing::MinimizerHashmapFragmentIndex::index_name;
constexpr std::string_view kMinimizersPerSequenceIndex =
    bioseq::indexing::index_descriptor<
        bioseq::indexing::MinimizerSequenceMergedIndex>::index_name;
constexpr sequence_size_t kPgSegmentationThresholdBp = 256'000'000;
constexpr sequence_size_t kOpportunisticIndexMinBp = 4096;

[[nodiscard]] inline bool pg_vulkan_seed_lookup_selected(
        const Minimap2Params& params) noexcept
{
    return params.vulkan_enabled && params.vulkan_seed_hits_enabled;
}

[[nodiscard]] inline std::uint64_t pg_gpu_index_resident_limit_bytes()
{
    bioseq::gpu::vulkan::Runtime& runtime =
        bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
    std::uint64_t limit = runtime.usable()
        ? runtime.features().device_local_bytes / 2u
        : (std::uint64_t{512} << 20);
    if (limit < (std::uint64_t{512} << 20)) {
        limit = std::uint64_t{512} << 20;
    }
    if (limit > (std::uint64_t{4} << 30)) {
        limit = std::uint64_t{4} << 30;
    }
    return limit;
}

[[nodiscard]] inline bioseq::indexing::GpuIndexCacheIdentity
pg_gpu_index_identity(
        std::uint64_t rowno,
        std::string_view index_name,
        sequence_size_t clip_lo_bp,
        sequence_size_t clip_hi_bp,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend sketch_backend) noexcept
{
    const auto scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(sketch_backend);
    std::uint64_t h = bioseq::indexing::splitmix64(rowno);
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(clip_lo_bp) + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(clip_hi_bp) + (h << 1));
    for (unsigned char c : index_name) {
        h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(c) + (h << 1));
    }
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(sketch_backend) + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(scheme.id) + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(scheme.version) + (h << 1));
    if (h == 0) {
        h = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::GpuIndexCacheIdentity{.assembly_identity = h};
}

[[nodiscard]] inline bioseq::indexing::GpuIndexCacheIdentity
pg_gpu_index_identity(
        const bioseq::storage::indexing::AssembledIndexKey& key) noexcept
{
    std::uint64_t h = bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.db_oid));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.user_relid) + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.tablespace_oid) + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.main_relfilenode) + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(key.column_attno)) + (h << 1));
    h ^= bioseq::indexing::splitmix64(key.index_name_hash + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.sketch_backend) + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.minimizer_scheme.id) + (h << 1));
    h ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.minimizer_scheme.version) + (h << 1));
    h ^= bioseq::indexing::splitmix64(key.query_key + (h << 1));
    h ^= bioseq::indexing::splitmix64(key.query_signature + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_lo_bp) + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_hi_bp) + (h << 1));
    if (h == 0) {
        h = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::GpuIndexCacheIdentity{.assembly_identity = h};
}

enum class DeferredMinimizerIndexKind : std::uint8_t {
    per_fragment,
    per_sequence,
};

struct DeferredMinimizerPersistChoice {
    DeferredMinimizerIndexKind kind = DeferredMinimizerIndexKind::per_fragment;
    std::string_view index_name = kMinimizersPerFragmentIndex;
};

[[nodiscard]] DeferredMinimizerIndexKind deferred_kind_for_index(
        std::string_view index_name) noexcept {
    return index_name == kMinimizersPerSequenceIndex
        ? DeferredMinimizerIndexKind::per_sequence
        : DeferredMinimizerIndexKind::per_fragment;
}

[[nodiscard]] std::optional<DeferredMinimizerPersistChoice>
choose_deferred_minimizer_persist_index(
        const BatchProcessor::ScannerPgContext& ctx) {
    if (!OidIsValid(ctx.user_relid) || ctx.mmsearch_seq_attno <= 0) {
        return std::nullopt;
    }

    constexpr auto minimizers = bioseq::indexing::MINIMIZER_INDICES;
    std::array<bool, minimizers.size()> enabled{};
    std::array<bool, minimizers.size()> update_on_insert{};
    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        enabled[i] = true;
        update_on_insert[i] =
            bioseq::indexing::default_update_on_insert_contains(minimizers[i]);
    }

	const int connect_status = bioseq::pg_utils::invoke_pg_as_cpp(
		[] { return SPI_connect(); });
	if (connect_status != SPI_OK_CONNECT) {
		bioseq::storage::indexing::active_read_policy_error_as_cpp(
			"deferred-persist SPI_connect failed",
			fmt::format("SPI rc={}", connect_status));
	}
	const std::string sql = fmt::format(
		"SELECT index_name, enabled, update_on_insert "
		"FROM bioseq_internal.index_policy "
		"WHERE user_relid={}::oid AND main_relfilenode={}::oid "
		"AND column_attno={}",
		static_cast<unsigned>(ctx.user_relid),
		static_cast<unsigned>(ctx.main_relfilenode),
		static_cast<int>(ctx.mmsearch_seq_attno));
	const int execute_status = bioseq::pg_utils::invoke_pg_as_cpp(
		[&sql] { return SPI_execute(sql.c_str(), true, 0); });
	std::optional<std::string> semantic_error;
	if (execute_status == SPI_OK_SELECT) {
		const std::uint64_t row_count =
			static_cast<std::uint64_t>(SPI_processed);
		for (std::uint64_t row = 0; row < row_count; ++row) {
			char* name_value = bioseq::pg_utils::invoke_pg_as_cpp([row] {
				return SPI_getvalue(
					SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1);
			});
			if (name_value == nullptr) {
				semantic_error = "deferred-persist policy index_name is NULL";
				break;
			}
			const std::string_view name{name_value};
			bool enabled_is_null = false;
			const Datum enabled_value = bioseq::pg_utils::invoke_pg_as_cpp(
				[row, &enabled_is_null] {
					return SPI_getbinval(
						SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2,
						&enabled_is_null);
				});
			bool update_is_null = false;
			const Datum update_value = bioseq::pg_utils::invoke_pg_as_cpp(
				[row, &update_is_null] {
					return SPI_getbinval(
						SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 3,
						&update_is_null);
				});
			if (enabled_is_null || update_is_null) {
				semantic_error =
					"deferred-persist policy boolean is NULL";
			} else {
				for (std::size_t i = 0; i < minimizers.size(); ++i) {
					if (minimizers[i] == name) {
						enabled[i] = DatumGetBool(enabled_value);
						update_on_insert[i] = DatumGetBool(update_value);
						break;
					}
				}
			}
			bioseq::pg_utils::invoke_pg_as_cpp(
				[name_value] { pfree(name_value); });
			if (semantic_error.has_value()) {
				break;
			}
		}
	} else {
		semantic_error = fmt::format(
			"deferred-persist policy query returned SPI rc={}",
			execute_status);
	}
	const int finish_status = bioseq::pg_utils::invoke_pg_as_cpp(
		[] { return SPI_finish(); });
	if (finish_status != SPI_OK_FINISH) {
		bioseq::storage::indexing::active_read_policy_error_as_cpp(
			"deferred-persist SPI_finish failed",
			fmt::format("SPI rc={}", finish_status));
	}
	if (semantic_error.has_value()) {
		bioseq::storage::indexing::active_read_policy_error_as_cpp(
			"deferred-persist policy result is invalid", *semantic_error);
	}

    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        if (enabled[i] && update_on_insert[i]) {
            return DeferredMinimizerPersistChoice{
                .kind = deferred_kind_for_index(minimizers[i]),
                .index_name = minimizers[i],
            };
        }
    }
    return std::nullopt;
}

#if 0  // Prohibited: biological index payloads may not be staged outside PostgreSQL.
[[nodiscard]] std::string make_async_persist_temp_path(
        std::string_view index_name,
        std::uint64_t sequence_rowno) {
    const char* prefix =
        index_name == kMinimizersPerSequenceIndex ? "mps" : "mpf";
    return fmt::format(
        "/tmp/bioseq_async_{}_{}_{}_{}.bin",
        prefix,
        static_cast<long long>(::getpid()),
        static_cast<unsigned long long>(sequence_rowno),
        static_cast<unsigned long long>(bioseq::instr::clock::now_ns()));
}
#endif

/*
 * Scale k/w for the small-query regime typical inside Postgres — short
 * motifs vs per-row sequences under 1 kb. Runs once per row before the
 * core pipeline. Factored out of the hot lambda so all code paths
 * (recompute vs prebuilt index) share a single size-heuristic knob and
 * so EXPLAIN can replay the same derivation for its param printout.
 *
 * The heuristic only fires when the user left the fields at their
 * minimap2 defaults. Any aligner_options() override or non-default GUC
 * value short-circuits the adjustment so we never silently overwrite a
 * user-supplied tuning value.
 */
[[nodiscard]] inline Minimap2Params apply_size_heuristic(
    const Minimap2Params& base, size_t min_bp)
{
    return BatchProcessor::effective_params_for_pair(base, min_bp);
}

[[nodiscard]] inline bool selected_vulkan_execution(
        const Minimap2Params& params) noexcept {
    return params.vulkan_enabled
        && (params.vulkan_seed_hits_enabled
            || params.minimizer_algorithm
                == MinimizerAlgorithmType::VULKAN_MINIMAP_INT
            || params.chaining_algorithm
                == ChainingAlgorithmType::VULKAN_TILED_DP);
}

[[nodiscard]] inline bool selected_vulkan_coordinate_prereqs_active(
        const Minimap2Params& params) noexcept {
    return params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && params.vulkan_fused_frontend_enabled
        && params.minimizer_algorithm
            == MinimizerAlgorithmType::VULKAN_MINIMAP_INT
        && params.chaining_algorithm
            == ChainingAlgorithmType::VULKAN_TILED_DP
        && !params.compute_divergence
        && !params.compute_mapq
        && params.max_occ >= 0;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::ExecutedBackend
backend_from_selected_params(const Minimap2Params& params) noexcept {
    const bool full_vulkan_pipeline =
        pipe::phase_c_index_build_active(params)
        && (pipe::phase_c_coordinate_only_active(params)
            || (params.do_cigar
                && params.vulkan_extend_enabled
                && selected_vulkan_coordinate_prereqs_active(params)));
    if (full_vulkan_pipeline) {
        return ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu;
    }
    if (selected_vulkan_execution(params)) {
        return ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid;
    }
    if (params.minimizer_algorithm == MinimizerAlgorithmType::TWO_STACK_NTHASH
        && params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2
        && params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
        return ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu;
    }
    if (params.minimizer_algorithm
            == MinimizerAlgorithmType::PAPER_NTHASH_SIMD
        && params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2
        && params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
        return ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu;
    }
    if (params.minimizer_algorithm == MinimizerAlgorithmType::SCALAR
        && params.chaining_algorithm == ChainingAlgorithmType::SCALAR_MM2
        && params.base_alignment_algorithm == BaseAlignmentAlgorithmType::KSW2) {
        return ::bioseq::pipeline_stats::ExecutedBackend::scalar_cpu;
    }
    return ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid;
}

inline void add_operational_counter(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        std::uint64_t& destination,
        const std::uint64_t increment = 1) noexcept {
    stats.operational.stats_saturated =
        ::bioseq::pipeline_stats::merge_counter(destination, increment)
        || stats.operational.stats_saturated;
}

inline void add_stage_host_time(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const ::bioseq::pipeline_stats::PipelineStage stage,
        const std::uint64_t elapsed_ns) noexcept {
    auto& destination = stats.operational.timing[
        ::bioseq::pipeline_stats::stage_index(stage)].host_wall_ns;
    add_operational_counter(stats, destination, elapsed_ns);
}

inline void record_unattested_job(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const Minimap2Params& params,
        const ::bioseq::pipeline_stats::TargetIndexSource requested_source,
        const ::bioseq::pipeline_stats::TargetIndexSource actual_source,
        const BatchProcessor::ScannerPgContext* scanner_pg_ctx,
        const std::size_t row_index,
        const bool per_row_query_mode,
        const std::uint64_t broadcast_query_ordinal,
        const MatchResultCollection* execution_proof,
        const bool consumed_shared_cached_query_proof,
        bool& cached_query_minimizer_proof_operation_accounted) noexcept {
    ::bioseq::pipeline_stats::PipelineJobExecutionStats job{};
    job.requested_backend = backend_from_selected_params(params);
    const bool selected_all_simd =
        job.requested_backend
            == ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu;
    const bool require_target_sketch =
        actual_source
            == ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    const bool accepted_all_simd = selected_all_simd
        && execution_proof != nullptr
        && execution_proof->all_simd_execution_accepted(
            params.minimizer_algorithm,
            require_target_sketch,
            /*require_nonempty_vector_work=*/true);
    // A broadcast persisted-query sketch is one physical operation shared by
    // logical jobs. Account it at its first proof-valid consumer even when
    // that job later has no chain/alignment; downstream route attestation is
    // a separate question. Recompute rows cannot enter this block because
    // their consumed_shared_cached_query_proof flag is false.
    const bool accepted_shared_query_sketch =
        consumed_shared_cached_query_proof
        && !cached_query_minimizer_proof_operation_accounted
        && execution_proof != nullptr
        && execution_proof->selected_simd_sketch_execution_accepted(
            params.minimizer_algorithm,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/false);
    if (accepted_shared_query_sketch) {
        if (params.minimizer_algorithm
                == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                execution_proof->exact_simd_query_execution_proof
                    .vector_batches);
        } else if (params.minimizer_algorithm
                    == MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                execution_proof->paper_nthash_query_execution_proof
                    .packed_decode_batches);
        }
        cached_query_minimizer_proof_operation_accounted = true;
    }
    // A SIMD identity is published only after the per-job collection proves
    // ExactSimd sketching, local SIMD score/backtracking, and Block alignment.
    // Every partial/missing/tampered proof remains mixed-invalid.
    job.actual_backend = accepted_all_simd
        ? ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu
        : ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid;
    job.requested_target_index_source = requested_source;
    job.actual_target_index_source = actual_source;
    const std::size_t effective_workers =
        bioseq::parallelism::GlobalPool::instance().max_concurrency();
    job.environment.effective_cpu_workers = effective_workers
            > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(effective_workers);
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->target_rownos[row_index] >= 0) {
        job.logical_target_row = static_cast<std::uint64_t>(
            scanner_pg_ctx->target_rownos[row_index]);
    } else {
        job.logical_target_row = static_cast<std::uint64_t>(row_index);
    }
    job.query_ordinal = scanner_pg_ctx != nullptr
            && scanner_pg_ctx->explicit_query_ordinals
        ? scanner_pg_ctx->query_ordinals[row_index]
        : per_row_query_mode
            ? static_cast<std::uint64_t>(row_index)
            : broadcast_query_ordinal;
    if (accepted_all_simd) {
        const auto mark = [&job](
                ::bioseq::pipeline_stats::PipelineStage stage,
                ::bioseq::pipeline_stats::ExecutionEngine engine) noexcept {
            auto& coverage = job.stages.coverage[
                ::bioseq::pipeline_stats::stage_index(stage)];
            coverage.eligible_jobs = 1;
            coverage.executed_jobs[
                ::bioseq::pipeline_stats::engine_index(engine)] = 1;
        };
        using Stage = ::bioseq::pipeline_stats::PipelineStage;
        using Engine = ::bioseq::pipeline_stats::ExecutionEngine;
        mark(Stage::query_sketch, Engine::simd_cpu);
        if (require_target_sketch) {
            mark(Stage::target_sketch, Engine::simd_cpu);
            mark(Stage::target_index_build, Engine::scalar_cpu);
        }
        mark(Stage::chain_score, Engine::simd_cpu);
        mark(Stage::chain_backtrack, Engine::simd_cpu);
        const auto& block = execution_proof->block_aligner_execution_proof;
        if (block.left_executed != 0) mark(Stage::left_extension, Engine::simd_cpu);
        if (block.gap_executed != 0) mark(Stage::gap_extension, Engine::simd_cpu);
        if (block.right_executed != 0) mark(Stage::right_extension, Engine::simd_cpu);

        if (params.minimizer_algorithm
                == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
            if (!consumed_shared_cached_query_proof) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_proof->exact_simd_query_execution_proof
                        .vector_batches);
            }
            if (execution_proof->exact_simd_target_execution_proof_present) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_proof->exact_simd_target_execution_proof.vector_batches);
            }
        } else if (params.minimizer_algorithm
                    == MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
            if (!consumed_shared_cached_query_proof) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_proof->paper_nthash_query_execution_proof
                        .packed_decode_batches);
            }
            if (execution_proof->paper_nthash_target_execution_proof_present) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_proof->paper_nthash_target_execution_proof
                        .packed_decode_batches);
            }
        }
        add_operational_counter(
            stats, stats.operational.simd_chain_vector_batches,
            execution_proof->simd_chaining_execution_proof.score_vector_batches);
        add_operational_counter(
            stats, stats.operational.simd_chain_backtrack_vector_batches,
            execution_proof->simd_chaining_execution_proof
                .backtrack_path_vector_batches);
    }
    // Non-SIMD and incomplete SIMD paths deliberately remain unattested.
    // Complete recomputed SIMD jobs carry only stages backed by local proof;
    // persisted identity/stage coverage remains fail-closed until its reader
    // publishes the generation proof.
    stats.merge_job(job);
}

inline void record_target_row(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const std::uint64_t rows = 1) noexcept {
    add_operational_counter(stats, stats.operational.target_rows, rows);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::TargetIndexSource
requested_target_index_source(
        const BatchProcessor::ScannerPgContext* scanner_pg_ctx) noexcept {
    if (scanner_pg_ctx == nullptr) {
        return ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    }
    switch (scanner_pg_ctx->index_read_requirement) {
        case bioseq::indexing::IndexReadRequirement::require_absent:
            return ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
        case bioseq::indexing::IndexReadRequirement::
                require_compatible_persisted:
            return ::bioseq::pipeline_stats::TargetIndexSource::persisted;
        case bioseq::indexing::IndexReadRequirement::allow:
            return scanner_pg_ctx->read_index_active
                ? ::bioseq::pipeline_stats::TargetIndexSource::persisted
                : ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    }
    return ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
}

[[noreturn]] inline void raise_required_persisted_index_error(
        const BatchProcessor::ScannerPgContext* scanner_pg_ctx,
        const std::int64_t target_row,
        std::string_view reason) {
    const std::string_view active_index =
        scanner_pg_ctx != nullptr && !scanner_pg_ctx->active_index.empty()
            ? std::string_view{scanner_pg_ctx->active_index}
            : std::string_view{"<unresolved>"};
	const std::string_view policy_source = scanner_pg_ctx != nullptr
		? bioseq::storage::indexing::
			active_read_policy_resolution_source_name(
				scanner_pg_ctx->active_index_policy_source)
		: std::string_view{"no_scanner_context"};
    bioseq_report<ERR, PG>()
        .code(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE)
        .msg("required compatible persisted minimizer index unavailable")
        .detail(
            "index_read_requirement=require_compatible_persisted "
			"target_row={} active_index={} active_index_policy_source={} reason={}",
            static_cast<long long>(target_row),
            active_index,
			policy_source,
            reason)
        .hint(
            "Publish and wire a validated revision-3 READY generation reader, "
            "or choose index_read_requirement=allow/require_absent; pair-sort "
            "rebuild and recompute fallbacks are forbidden.");
    __builtin_unreachable();
}

inline void record_target_index_outcome(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const ::bioseq::pipeline_stats::TargetIndexSource requested_source,
        const ::bioseq::pipeline_stats::TargetIndexSource actual_source,
        const std::uint64_t rows = 1) noexcept {
    if (actual_source
        == ::bioseq::pipeline_stats::TargetIndexSource::persisted) {
        add_operational_counter(
            stats, stats.operational.persisted_index_rows, rows);
    } else if (actual_source
               == ::bioseq::pipeline_stats::TargetIndexSource::recomputed) {
        add_operational_counter(
            stats, stats.operational.recomputed_target_index_rows, rows);
    }
    if (requested_source
            == ::bioseq::pipeline_stats::TargetIndexSource::persisted
        && actual_source
            != ::bioseq::pipeline_stats::TargetIndexSource::persisted) {
        add_operational_counter(
            stats, stats.operational.missing_persisted_index_rows, rows);
    }
}

template <class Seq>
[[nodiscard]] std::span<const std::uint32_t> append_gpu_alignment_symbols(
        std::vector<std::uint32_t>& destination,
        std::vector<typename Seq::traits::letter_unit>& scratch,
        const Seq& seq,
        sequence_size_t start_bp,
        sequence_size_t end_bp,
        bool reverse)
{
    using traits = typename Seq::traits;
    using rank_t = typename traits::letter_unit;

    if (end_bp < start_bp) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "GPU CIGAR extension received inverted sequence window");
    }
    const sequence_size_t len_bp = end_bp - start_bp;
    if (len_bp > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "GPU CIGAR extension window length {} exceeds simple CIGAR limit {}",
            len_bp, bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len));
    }
    if (end_bp > static_cast<sequence_size_t>(seq.bp_size())) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "GPU CIGAR extension sequence window exceeds sequence length");
    }

    const std::size_t start = static_cast<std::size_t>(start_bp);
    const std::size_t end = static_cast<std::size_t>(end_bp);
    const std::size_t len = end - start;
    const std::size_t out_offset = destination.size();
    destination.resize(out_offset + len);
    if (len == 0) {
        return std::span<const std::uint32_t>{destination.data() + out_offset, 0u};
    }

    if constexpr (sizeof(rank_t) == sizeof(std::uint8_t)) {
        scratch.resize(len);
        auto out8 = std::span<std::uint8_t>{
            reinterpret_cast<std::uint8_t*>(scratch.data()), len};
        if (reverse) {
            seq.template to_encoded_into<false, true>(out8, start, end);
            if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                for (rank_t& rank : scratch) {
                    rank = static_cast<rank_t>(
                        traits::letter_complement(
                            static_cast<typename traits::storage_unit>(rank)));
                }
            }
        } else {
            seq.template to_encoded_into<false, false>(out8, start, end);
        }
        for (std::size_t i = 0; i < len; ++i) {
            destination[out_offset + i] = static_cast<std::uint32_t>(scratch[i]);
        }
    } else {
        auto iter = seq.template iterable<BP>().begin();
        for (std::size_t i = 0; i < start; ++i) {
            ++iter;
        }
        if (reverse) {
            for (std::size_t i = 0; i < len; ++i) {
                const auto letter = *iter;
                if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                    destination[out_offset + len - 1u - i] = static_cast<std::uint32_t>(
                        traits::letter_complement(
                            static_cast<typename traits::storage_unit>(letter)));
                } else {
                    destination[out_offset + len - 1u - i] =
                        static_cast<std::uint32_t>(letter);
                }
                ++iter;
            }
        } else {
            for (std::size_t i = 0; i < len; ++i) {
                destination[out_offset + i] = static_cast<std::uint32_t>(*iter);
                ++iter;
            }
        }
    }

    return std::span<const std::uint32_t>{destination.data() + out_offset, len};
}

[[nodiscard]] inline std::uint32_t gpu_alignment_common_match_run(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::uint32_t query_pos,
        std::uint32_t target_pos) noexcept
{
    std::uint32_t run = 0u;
    while (query_pos + run < query_symbols.size()
           && target_pos + run < target_symbols.size()
           && query_symbols[query_pos + run] == target_symbols[target_pos + run]) {
        ++run;
    }
    return run;
}

[[nodiscard]] inline bool gpu_alignment_unit_gap_walk_cigar_candidate(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols) noexcept
{
    const std::uint32_t query_len =
        static_cast<std::uint32_t>(query_symbols.size());
    const std::uint32_t target_len =
        static_cast<std::uint32_t>(target_symbols.size());
    if (query_len == target_len) {
        return false;
    }

    const int net_query = static_cast<int>(query_len) - static_cast<int>(target_len);
    const int abs_net_query = net_query < 0 ? -net_query : net_query;
    if (abs_net_query
        > static_cast<int>(
            bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_gap_max)) {
        return false;
    }

    std::uint32_t query_pos = 0u;
    std::uint32_t target_pos = 0u;
    std::uint32_t gap_count = 0u;

    while (query_pos < query_len || target_pos < target_len) {
        std::uint32_t run_len = 0u;
        if (query_pos < query_len && target_pos < target_len) {
            run_len = gpu_alignment_common_match_run(
                query_symbols, target_symbols, query_pos, target_pos);
        }
        if (run_len != 0u) {
            query_pos += run_len;
            target_pos += run_len;
            continue;
        }

        if (gap_count
            >= bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_gap_op_max) {
            return false;
        }
        if (query_pos == query_len) {
            target_pos = target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == target_len) {
            query_pos = query_len;
            ++gap_count;
            continue;
        }

        if (target_pos + 1u < target_len
            && query_symbols[query_pos] == target_symbols[target_pos + 1u]) {
            ++target_pos;
            ++gap_count;
            continue;
        }
        if (query_pos + 1u < query_len
            && query_symbols[query_pos + 1u] == target_symbols[target_pos]) {
            ++query_pos;
            ++gap_count;
            continue;
        }

        return false;
    }

    return gap_count != 0u;
}

inline void append_packed_cigar_slice(
        std::vector<CigarOp>& destination,
        std::span<const std::uint32_t> packed)
{
    for (std::uint32_t entry : packed) {
        const std::uint32_t opcode = entry & 0xfU;
        const std::uint32_t op_length = entry >> 4;
        if (op_length == 0u) {
            continue;
        }
        if (!destination.empty() && destination.back().opcode == opcode) {
            destination.back().length += op_length;
        } else {
            destination.push_back(CigarOp{op_length, opcode});
        }
    }
}

[[noreturn]] inline void raise_vulkan_pg_error(
        std::string_view stage,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_INTERNAL_ERROR)
        .msg("Vulkan aligner execution failed")
        .detail("{}: {}", stage, detail)
        .hint("Selected Vulkan execution has no CPU fallback; fix the Vulkan runtime or select a non-Vulkan algorithm before dispatch.");
    __builtin_unreachable();
}

[[noreturn]] inline void raise_exact_simd_pg_error(
        std::string_view stage,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_FEATURE_NOT_SUPPORTED)
        .msg("Exact SIMD minimizer execution is unsupported for this request")
        .detail("{}: {}", stage, detail)
        .hint("Exact SIMD has no scalar fallback; use DNA4 with a supported packed storage width and supported W/K/vector shape, or select a different algorithm tuple explicitly.");
    __builtin_unreachable();
}

[[noreturn]] inline void raise_paper_nthash_pg_error(
        std::string_view stage,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_FEATURE_NOT_SUPPORTED)
        .msg("Paper ntHash SIMD minimizer execution is unsupported for this request")
        .detail("{}: {}", stage, detail)
        .hint("Paper ntHash SIMD has no scalar or Exact SIMD fallback; use DNA4 with packed uint32/uint64 storage, W=10, K=15, and an admissible range, or select another algorithm explicitly.");
    __builtin_unreachable();
}

[[noreturn]] inline void raise_simd_chaining_pg_error(
        std::string_view stage,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_FEATURE_NOT_SUPPORTED)
        .msg("Exact SIMD chaining execution is unsupported for this request")
        .detail("{}: {}", stage, detail)
        .hint("Exact SIMD chaining has no scalar fallback; use a supported SIMD target and valid chaining parameters, or select a different chaining algorithm explicitly.");
    __builtin_unreachable();
}

[[noreturn]] inline void raise_persisted_minimizer_lookup_pg_error(
        std::string_view stage,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_DATA_CORRUPTED)
        .msg("Persisted minimizer lookup is incomplete or inconsistent")
        .detail("{}: {}", stage, detail)
        .hint("Rebuild the compatible persisted index generation; this path has no recompute or scalar fallback.");
    __builtin_unreachable();
}

template <std::size_t Capacity>
[[nodiscard]] inline std::size_t copy_pg_error_detail(
        std::array<char, Capacity>& destination,
        const char* source) noexcept {
    static_assert(Capacity > 0u);
    if (source == nullptr) {
        destination[0] = '\0';
        return 0u;
    }
    std::size_t size = 0u;
    while (size + 1u < Capacity && source[size] != '\0') {
        destination[size] = source[size];
        ++size;
    }
    destination[size] = '\0';
    return size;
}

template <class Fn>
[[nodiscard]] MatchResultCollection run_aligner_pg_checked(
        std::string_view stage,
        Fn&& fn) {
    enum class FailureKind : std::uint8_t {
        none,
        vulkan,
        exact_simd,
        paper_nthash,
        simd_chaining,
        persisted_minimizer_lookup,
    };
    FailureKind failure = FailureKind::none;
    // PostgreSQL ERROR is a longjmp. Keep only trivially destructible state
    // alive after the C++ catch blocks; a std::string here would have its
    // destructor skipped by every raise_*_pg_error call below.
    std::array<char, 2048> detail{};
    std::size_t detail_size = 0u;
    try {
        return std::forward<Fn>(fn)();
    } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& err) {
        failure = FailureKind::vulkan;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::ExactSimdRuntimeError& err) {
        failure = FailureKind::exact_simd;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::PaperNtHashRuntimeError& err) {
        failure = FailureKind::paper_nthash;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::SimdChainingRuntimeError& err) {
        failure = FailureKind::simd_chaining;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const bioseq::indexing::PersistedMinimizerLookupError& err) {
        failure = FailureKind::persisted_minimizer_lookup;
        detail_size = copy_pg_error_detail(detail, err.what());
    }
    // The C++ exception object has been destroyed, and only the fixed buffer,
    // length, enum, and string_view arguments remain across PostgreSQL ERROR.
    const std::string_view detail_view{detail.data(), detail_size};
    switch (failure) {
        case FailureKind::vulkan:
			bioseq::pg_utils::invoke_pg_as_cpp(
				[stage, detail_view] {
					raise_vulkan_pg_error(stage, detail_view);
				});
			break;
        case FailureKind::exact_simd:
			bioseq::pg_utils::invoke_pg_as_cpp(
				[stage, detail_view] {
					raise_exact_simd_pg_error(stage, detail_view);
				});
			break;
        case FailureKind::paper_nthash:
			bioseq::pg_utils::invoke_pg_as_cpp(
				[stage, detail_view] {
					raise_paper_nthash_pg_error(stage, detail_view);
				});
			break;
        case FailureKind::simd_chaining:
			bioseq::pg_utils::invoke_pg_as_cpp(
				[stage, detail_view] {
					raise_simd_chaining_pg_error(stage, detail_view);
				});
			break;
        case FailureKind::persisted_minimizer_lookup:
			bioseq::pg_utils::invoke_pg_as_cpp(
				[stage, detail_view] {
					raise_persisted_minimizer_lookup_pg_error(
						stage, detail_view);
				});
			break;
        case FailureKind::none:
            break;
    }
    __builtin_unreachable();
}

template <class... Args>
[[nodiscard]] MatchResultCollection minimap_port_pg_checked(Args&&... args) {
    return run_aligner_pg_checked(
        "minimap_port",
        [&args...]() -> MatchResultCollection {
            return minimap_port(std::forward<Args>(args)...);
        });
}

template <class... Args>
[[nodiscard]] MatchResultCollection minimap_port_segmented_pg_checked(
        Args&&... args) {
    return run_aligner_pg_checked(
        "minimap_port_segmented",
        [&args...]() -> MatchResultCollection {
            return minimap_port_segmented(std::forward<Args>(args)...);
        });
}

template <class... Args>
[[nodiscard]] MatchResultCollection minimap2_port_with_index_pg_checked(
        Args&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index",
        [&args...]() -> MatchResultCollection {
            return minimap2_port_with_index(std::forward<Args>(args)...);
        });
}

struct OpportunisticIndexChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

struct OpportunisticFragmentIndexStorage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<OpportunisticIndexChunk> chunks{};

    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq_log_progress<ERR, PG>( "bioseq opportunistic index: {}", msg);
        abort();
    }

    [[nodiscard]] bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedHeader
    validate_minimizer_blob_or_error(
            const char* p,
            std::size_t bytes,
            std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) {
            index_error("minimizer blob truncated header");
        }
        H h{};
        std::memcpy(&h, p, sizeof(H));
        if (h.magic != bioseq::indexing::MinimizerHashmapFragmentIndex::kSerializeMagic
            || h.version != bioseq::indexing::MinimizerHashmapFragmentIndex::kSerializeVersion) {
            index_error("minimizer blob magic/version");
        }
        const std::size_t expect =
            sizeof(H) + sizeof(B) * h.bucket_count + sizeof(std::uint64_t) * h.total_positions;
        if (bytes != expect) {
            index_error("minimizer blob size mismatch");
        }
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return h;
    }

    void validate_minimizer_directory_or_error(const char*, std::size_t bytes) const {
        if (bytes < sizeof(std::int64_t)) {
            index_error("directory truncated");
        }
    }

    void validate_minimizer_chunk_metadata_or_error(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::size_t min_bytes,
            std::size_t) const {
        if (chunk_byte_size(sub_key, chunk_no) < min_bytes) {
            index_error("chunk metadata below minimum size");
        }
    }

    [[nodiscard]] std::size_t number_of_chunks(
            bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t count = 0;
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (!have || chunk.sub_key != last) {
                ++count;
                last = chunk.sub_key;
                have = true;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_byte_size(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) const {
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) {
        for (OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return reinterpret_cast<const T*>(chunk.bytes.data());
            }
        }
        index_error("chunk missing");
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0
                ? 1
                : (bytes_count + bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES - 1)
                    / bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES);
            plan[i] = bytes;
            remaining -= bytes;
        }
        plans[sub_key] = std::move(plan);
    }

    void resize_chunks(
            bioseq::indexing::index_sub_key_t sub_key,
            std::span<const std::size_t> per_chunk_bytes) {
        plans[sub_key] = std::vector<std::size_t>(
            per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class T, class F>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(T) != 0) {
                index_error("chunk payload not a whole number of T");
            }
            OpportunisticIndexChunk chunk{};
            chunk.sub_key = sub_key;
            chunk.chunk_no = static_cast<std::uint32_t>(chunk_no);
            chunk.bytes.assign(payload_bytes, char{0});
            const std::size_t next =
                writer(chunk_no, reinterpret_cast<T*>(chunk.bytes.data()),
                       payload_bytes / sizeof(T));
            if (next < chunk_no) {
                index_error("writer moved backwards");
            }
            chunks.push_back(std::move(chunk));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(it);
    }

    template <class F>
    void read_chunks_in_range(
            bioseq::indexing::index_sub_key_t lo,
            bioseq::indexing::index_sub_key_t hi,
            F&& cb) {
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key >= lo && chunk.sub_key < hi) {
                cb(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    void for_each_sub_key(auto&& fn) const {
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (!have || chunk.sub_key != last) {
                fn(chunk.sub_key);
                last = chunk.sub_key;
                have = true;
            }
        }
    }

    void clear_entity() {
        plans.clear();
        chunks.clear();
    }

    void clear(std::string_view) {
        clear_entity();
    }

    void discard_sub_keys(
            bioseq::indexing::index_sub_key_t lo,
            bioseq::indexing::index_sub_key_t hi) {
        chunks.erase(
            std::remove_if(
                chunks.begin(), chunks.end(),
                [lo, hi](const OpportunisticIndexChunk& chunk) {
                    return chunk.sub_key >= lo && chunk.sub_key <= hi;
                }),
            chunks.end());
    }

    [[nodiscard]] std::size_t spill_sorted_run(
            std::span<const std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>>,
            std::uint64_t,
            bioseq::parallelism::MemoryPermit permit) {
        permit.release();
        index_error("external sort spill is not supported by opportunistic storage");
    }

    void merge_runs(auto&&) {
        index_error("external sort merge is not supported by opportunistic storage");
    }

    void release_resident_runs_noexcept() noexcept {}

    void release_runs_noexcept() noexcept {}

    void release_runs() {}

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return 0;
    }
};

// DOC0345 Patch J — transient mps storage backing a real no_index per_sequence
// build. Unlike OpportunisticFragmentIndexStorage (which refuses external-sort
// spill), this delegates spill/merge to a real SortedPairRunStore so it satisfies
// is_external_sort_spill_storage + is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
// that MinimizerSequenceMergedIndex<S> requires. No persisted sidecar rows.
struct OpportunisticSequenceChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::size_t chunk_no = 0;
    std::vector<char> bytes;
};

struct OpportunisticSequenceIndexStorage {
    using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

    std::vector<OpportunisticSequenceChunk> chunks;
    bioseq::storage::indexing::SortedPairRunStore sorted_runs;
    std::size_t run_buffer_bytes_value = std::size_t{256} << 20;

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return run_buffer_bytes_value;
    }

    void clear_entity() {
        chunks.clear();
        sorted_runs.release_runs();
    }

    // DOC0345 Patch J — required by is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
    // but unused on the transient path.
    void clear(std::string_view) {
        clear_entity();
    }

    void resize_chunks(
        bioseq::indexing::index_sub_key_t sub_key,
        std::span<const std::size_t> plan)
    {
        for (std::size_t chunk_no = 0; chunk_no < plan.size(); ++chunk_no) {
            auto& chunk = ensure_chunk(sub_key, chunk_no);
            chunk.bytes.resize(plan[chunk_no]);
        }
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes) {
        std::array<std::size_t, 1> plan{bytes};
        resize_chunks(sub_key, std::span<const std::size_t>{plan.data(), plan.size()});
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class T, class F>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        for (OpportunisticSequenceChunk& chunk : chunks) {
            if (chunk.sub_key != sub_key) {
                continue;
            }
            const std::size_t cap = chunk.bytes.size() / sizeof(T);
            writer(
                chunk.chunk_no,
                reinterpret_cast<T*>(chunk.bytes.data()),
                cap);
        }
    }

    template <class T, class F>
    void write_to(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        write_to_each<T>(sub_key, std::forward<F>(writer));
    }

    [[nodiscard]] const char* read_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        return chunk_ref(sub_key, chunk_no).bytes.data();
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        return reinterpret_cast<const T*>(chunk_ref(sub_key, chunk_no).bytes.data());
    }

    [[nodiscard]] std::size_t chunk_byte_size(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        return chunk_ref(sub_key, chunk_no).bytes.size();
    }

    [[nodiscard]] std::size_t number_of_chunks(
        bioseq::indexing::index_sub_key_t sub_key) const
    {
        std::size_t count = 0;
        for (const OpportunisticSequenceChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++count;
            }
        }
        return count;
    }

    // DOC0345 Patch J — distinct sub_keys present (required by the concept).
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t count = 0;
        for_each_sub_key([&count](bioseq::indexing::index_sub_key_t) { ++count; });
        return count;
    }

    template <class F>
    void for_each_sub_key(F&& fn) const {
        bioseq::indexing::index_sub_key_t last = 0;
        bool have_last = false;
        // chunks are appended in build order; emit each distinct sub_key once.
        std::vector<bioseq::indexing::index_sub_key_t> seen;
        for (const OpportunisticSequenceChunk& chunk : chunks) {
            if (have_last && chunk.sub_key == last) {
                continue;
            }
            bool already = false;
            for (const auto key : seen) {
                if (key == chunk.sub_key) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                seen.push_back(chunk.sub_key);
                fn(chunk.sub_key);
            }
            last = chunk.sub_key;
            have_last = true;
        }
    }

    [[nodiscard]] std::size_t spill_sorted_run(
        std::span<const sorted_pair_t> sorted)
    {
        return sorted_runs.spill_sorted_run(sorted);
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(
        std::vector<sorted_pair_t> sorted)
    {
        // The transient no-index MPS path immediately re-enters Phase C for the
        // next target range. Keep these runs file-backed so resident pair
        // vectors do not consume the admission budget needed by the nested GPU
        // sketch dispatch.
        return sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
        std::vector<sorted_pair_t> sorted,
        std::uint64_t admitted_bytes,
        bioseq::parallelism::MemoryPermit permit)
    {
        (void)admitted_bytes;
        const std::size_t run_index = sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
        permit.release();
        return run_index;
    }

    template <class F>
    void merge_runs(F&& on_kmer) {
        sorted_runs.merge_runs(std::forward<F>(on_kmer));
    }

    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    void release_runs() {
        sorted_runs.release_runs();
    }

    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
    }

    template <class F>
    void read_chunks_in_range(
        bioseq::indexing::index_sub_key_t first,
        bioseq::indexing::index_sub_key_t last,
        F&& f) const
    {
        for (bioseq::indexing::index_sub_key_t sub_key = first;
             sub_key < last;
             ++sub_key) {
            for (const OpportunisticSequenceChunk& chunk : chunks) {
                if (chunk.sub_key == sub_key) {
                    f(sub_key, chunk.bytes.data(), chunk.bytes.size());
                }
            }
        }
    }

    void validate_minimizer_directory_or_error(
        const char*,
        std::size_t) const
    {
    }

    void validate_minimizer_chunk_metadata_or_error(
        bioseq::indexing::index_sub_key_t,
        std::size_t,
        std::size_t,
        std::size_t) const
    {
    }

    auto validate_minimizer_blob_or_error(
        const char* blob,
        std::size_t bytes,
        std::uint32_t expected_bucket_count = 0) const
    {
        using Header = bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedHeader;
        if (bytes < sizeof(Header)) {
            index_error("opportunistic mps: truncated blob");
        }
        Header header{};
        std::memcpy(&header, blob, sizeof(header));
        if (expected_bucket_count != 0u
            && header.bucket_count != expected_bucket_count) {
            index_error("opportunistic mps: unexpected bucket count");
        }
        return header;
    }

    [[noreturn]] void index_error(std::string_view message) const {
		bioseq::pg_utils::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq opportunistic mps index: %.*s",
					static_cast<int>(message.size()), message.data())));
		});
		pg_unreachable();
    }

private:
    OpportunisticSequenceChunk& ensure_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no)
    {
        for (OpportunisticSequenceChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        chunks.push_back(OpportunisticSequenceChunk{
            .sub_key = sub_key,
            .chunk_no = chunk_no,
            .bytes = {},
        });
        return chunks.back();
    }

    [[nodiscard]] const OpportunisticSequenceChunk& chunk_ref(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        for (const OpportunisticSequenceChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
		const std::string detail = fmt::format(
			"missing chunk sub_key={} chunk_no={}",
			static_cast<long long>(sub_key), chunk_no);
		index_error(detail);
    }
};

#if 0  // Prohibited external-file producer; retained only as archaeology.
struct AsyncPersistFileChunkHeader {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t size;
};

static_assert(sizeof(AsyncPersistFileChunkHeader) == 24);

struct AsyncPersistFileFragmentStorage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::string path{};
    std::ofstream out{};
    std::uint64_t chunk_count = 0;
    std::uint64_t file_bytes = 0;
    Oid db_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t sequence_rowno = 0;
    bioseq::storage::indexing::SortedPairRunStore sorted_runs{};

    explicit AsyncPersistFileFragmentStorage(std::string p)
        : path(std::move(p)),
          out(path, std::ios::binary | std::ios::trunc) {
        if (!out) {
            bioseq_log<ERR, PG>(
                   "bioseq async persist: failed to open temp payload file {}",
                   path);
        }
    }

    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq_log_progress<ERR, PG>( "bioseq async persist file storage: {}", msg);
        abort();
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0
                ? 1
                : (bytes_count + bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES - 1)
                    / bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES);
            plan[i] = bytes;
            remaining -= bytes;
        }
        plans[sub_key] = std::move(plan);
    }

    void resize_chunks(
            bioseq::indexing::index_sub_key_t sub_key,
            std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t bytes : plan) {
            if (bytes > bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES) {
                index_error("chunk payload exceeds MAX_INDEX_CHUNK_SIZE_BYTES");
            }
        }
        plans[sub_key] = std::move(plan);
    }

    [[nodiscard]] std::size_t number_of_sub_keys() const {
        return plans.size();
    }

    [[nodiscard]] std::size_t number_of_chunks(
            bioseq::indexing::index_sub_key_t sub_key) const {
        if (auto it = plans.find(sub_key); it != plans.end()) {
            return it->second.size();
        }
        return 0;
    }

    [[nodiscard]] std::size_t chunk_byte_size(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) const {
        if (auto it = plans.find(sub_key);
            it != plans.end() && chunk_no < it->second.size()) {
            return it->second[chunk_no];
        }
        return 0;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(
            bioseq::indexing::index_sub_key_t,
            std::size_t) {
        index_error("read_chunk is not supported by async write-only storage");
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class F>
    void read_chunks_in_range(
            bioseq::indexing::index_sub_key_t,
            bioseq::indexing::index_sub_key_t,
            F&&) {
        index_error("read_chunks_in_range is not supported by async write-only storage");
    }

    template <class F>
    void for_each_sub_key(F&& fn) const {
        for (const auto& [sub_key, _] : plans) {
            fn(sub_key);
        }
    }

    void clear_entity() {
        plans.clear();
    }

    void clear(std::string_view) {
        clear_entity();
    }

    template <class T, class F>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(T) != 0) {
                index_error("chunk payload not a whole number of T");
            }
            std::vector<char> payload(payload_bytes, char{0});
            const std::size_t next =
                writer(chunk_no, reinterpret_cast<T*>(payload.data()),
                       payload_bytes / sizeof(T));
            if (next < chunk_no || next > plan.size()) {
                index_error("writer produced invalid next chunk");
            }

            const AsyncPersistFileChunkHeader header{
                .sub_key = static_cast<std::int64_t>(sub_key),
                .chunk_no = static_cast<std::uint32_t>(chunk_no),
                .reserved = 0,
                .size = static_cast<std::uint64_t>(payload_bytes),
            };
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!payload.empty()) {
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
            if (!out) {
                index_error("write failed");
            }
            ++chunk_count;
            file_bytes += sizeof(header) + payload_bytes;
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(it);
    }

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        if (bioseq_index_run_buffer_bytes > 0) {
            return static_cast<std::size_t>(bioseq_index_run_buffer_bytes);
        }
        constexpr std::uint64_t desired = std::uint64_t{512} << 20;
        constexpr std::uint64_t floor = std::uint64_t{64} << 20;
        const std::uint64_t cap =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .index_memory_cap_bytes();
        if (cap == 0) {
            return static_cast<std::size_t>(desired);
        }
        return static_cast<std::size_t>(
            std::max<std::uint64_t>(std::min(desired, cap), floor));
    }

    [[nodiscard]] std::size_t spill_sorted_run(
            std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
        return sorted_runs.spill_sorted_run(sorted);
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted) {
        return sorted_runs.spill_sorted_run_owned(std::move(sorted));
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
            std::uint64_t admitted_bytes,
            bioseq::parallelism::MemoryPermit permit) {
        return sorted_runs.spill_sorted_run_owned_admitted(
            std::move(sorted), admitted_bytes, std::move(permit));
    }

    template <class F>
    void merge_runs(F&& on_kmer) {
        sorted_runs.merge_runs(std::forward<F>(on_kmer));
    }

    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
    }

    void release_runs() {
        release_runs_noexcept();
    }

    [[nodiscard]] bioseq::storage::indexing::SortedRunStoreStats
    sorted_run_stats() const noexcept {
        return sorted_runs.stats();
    }

    void snapshot_sorted_run_stats_before_release() {
        const auto st = sorted_runs.stats();
        const std::uint64_t total_runs = sorted_runs.run_count();
        bioseq::storage::indexing::g_last_index_merge_run_count = total_runs;
        bioseq::storage::indexing::g_last_mps_build_stats =
            bioseq::storage::indexing::LastMpsBuildStats{
                .backend_pid = static_cast<std::uint64_t>(MyProcPid),
                .db_oid = static_cast<std::uint64_t>(db_oid),
                .relfilenode = static_cast<std::uint64_t>(main_relfilenode),
                .sequence_rowno = sequence_rowno,
                .total_run_count = total_runs,
                .resident_runs = st.resident_runs,
                .file_runs = st.file_runs,
                .resident_run_bytes = st.resident_bytes,
                .resident_peak_bytes = st.resident_peak_bytes,
                .spilled_run_bytes = st.spilled_bytes,
                .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
                .postgres_temp_file_count = st.postgres_temp_file_count,
                .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
                .merge_pass_count = st.merge_pass_count,
                .spill_count_from_pressure = st.spill_count_from_pressure,
                .resident_demotions = st.resident_demotions,
                .position_spill_bytes = st.position_spill_bytes,
                .position_spill_count = st.position_spill_count,
            };
        bioseq_log<DBG>("[bioseq.index.mps_build.row] backend_pid={} async_file=1 db_oid={} relfilenode={} sequence_rowno={} "
                "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
                "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
                "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
                "resident_demotions={} position_spill_bytes={} position_spill_count={}",
                MyProcPid,
                static_cast<unsigned long long>(db_oid),
                static_cast<unsigned long long>(main_relfilenode),
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(st.resident_runs),
                static_cast<unsigned long long>(st.file_runs),
                static_cast<unsigned long long>(st.resident_bytes),
                static_cast<unsigned long long>(st.resident_peak_bytes),
                static_cast<unsigned long long>(st.spilled_bytes),
                static_cast<unsigned long long>(st.postgres_temp_file_bytes),
                static_cast<unsigned long long>(st.postgres_temp_file_count),
                static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
                static_cast<unsigned long long>(st.merge_pass_count),
                static_cast<unsigned long long>(st.spill_count_from_pressure),
                static_cast<unsigned long long>(st.resident_demotions),
                static_cast<unsigned long long>(st.position_spill_bytes),
                static_cast<unsigned long long>(st.position_spill_count));
    }

    void finish() {
        out.flush();
        if (!out) {
            index_error("flush failed");
        }
        out.close();
    }

    void remove_noexcept() noexcept {
        try {
            if (out.is_open()) {
                out.close();
            }
            if (!path.empty()) {
                std::remove(path.c_str());
            }
            release_runs_noexcept();
        } catch (...) {
        }
    }
};
#endif

[[nodiscard]] bioseq::parallelism::pg::PersistTarget
make_fragment_index_persist_target(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::string_view index_name = kMinimizersPerFragmentIndex) {
    return bioseq::parallelism::pg::PersistTarget{
        .db_oid = ctx.db_oid,
        .user_relid = ctx.user_relid,
        .tablespace_oid = ctx.tablespace_oid,
        .main_relfilenode = ctx.main_relfilenode,
        .column_attno = ctx.mmsearch_seq_attno,
        .bucket_idx = static_cast<std::uint8_t>(loc.bucket_idx),
        .shard_idx = loc.shard_idx,
        .sequence_rowno = sequence_rowno,
        .index_name = index_name,
    };
}

#if 0  // Prohibited external-file producer; no finish/stage entry is compiled.
[[nodiscard]] bool finish_and_stage_async_index_file(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::string_view index_name,
        AsyncPersistFileFragmentStorage& file_store) {
    file_store.finish();
    const auto persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, index_name);
    return bioseq::parallelism::pg::stage_async_index_persist_file(
        persist_target,
        std::move(file_store.path),
        file_store.file_bytes,
        file_store.chunk_count);
}
#endif

[[nodiscard]] bool can_defer_recompute_minimizer_persist(
        const BatchProcessor::ScannerPgContext& ctx,
        const Minimap2Params& params) noexcept {
    return ctx.recompute_async_persist_active
        && ctx.index_async_persist_active
        && OidIsValid(ctx.user_relid)
        && params.kmer_size == 15
        && params.window_size == 10
        // Legacy async MPF/MPS payloads have no minimizer-scheme field.
        // Paper recompute remains Paper, but persistence must wait for the
        // scheme-tagged READY-generation writer; never relabel it Exact/Scalar.
        && pipe::target_index_sketch_backend_from_params(params)
            != bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
                PaperNtHashSimd;
}

// DOC0345 Patch J — select the target index sketch backend for recompute from
// the resolved params (Vulkan only when all three high-level gates are on).
[[nodiscard]] inline bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
target_backend_from_params_for_recompute(const Minimap2Params& params) noexcept
{
    return pipe::target_index_sketch_backend_from_params(params);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::SegmentUnitRange>
pg_segment_unit_ranges(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        const Minimap2Params& params)
{
    const auto backend =
        (pg_vulkan_seed_lookup_selected(params)
         || target_backend_from_params_for_recompute(params)
             == bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::VulkanSegment)
            ? bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::VulkanSegment
            : bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
    return pipe::segment_unit_ranges_for_backend(
        total_bp,
        query_len,
        lpu,
        backend);
}

// DOC0345 Patch J — build a REAL transient minimizers_per_sequence index
// (GPU-backed when Vulkan is selected), assemble it for the query, and run the
// indexed aligner. Shared by BOTH the fast no_index recompute path and the
// deferred-persist recompute path, so neither serves a no_index x per_sequence
// cell as an mpf-shaped segmented relabel (DOC0345 §1.4, §2). Streams from the
// (lazy PG) target via the GPU sorted-pair seam (GPU Phase B emit on device +
// host pair sort — see DOC0348) — no whole-target resident materialization, so
// 2/4 Gbp targets do not OOM.
template <class LazyQuery, class Target>
[[nodiscard]] MatchResultCollection build_and_align_transient_mps(
    LazyQuery& lazy_query,
    Target& target,
    const Minimap2Params& params,
    sequence_size_t total_bp_size)
{
    MatchResultCollection collection;
	// Every PostgreSQL terminal error below crosses a true
	// invoke_pg_as_cpp leaf. Keep this orchestration in ordinary C++ so MPS
	// chunks, sorted runs, GPU cache, query proof, assembled indexes, and
	// segment results unwind normally on PostgresErrorAsCpp.
	OpportunisticSequenceIndexStorage mps_store;
        mps_store.run_buffer_bytes_value =
            bioseq_index_run_buffer_bytes > 0
                ? static_cast<std::size_t>(bioseq_index_run_buffer_bytes)
                : (std::size_t{256} << 20);

        // MinimizerSequenceMergedIndex::build is pinned to w=10,k=15. The
        // transient read side must use the same minimizer parameters even if
        // apply_size_heuristic() reduced the caller's k/w for short reads.
        // Keep the caller's scoring/seed-count adjustments.
        Minimap2Params mps_params = params;
        mps_params.kmer_size = 15;
        mps_params.window_size = 10;

        const auto backend = target_backend_from_params_for_recompute(mps_params);
        bioseq::indexing::MinimizerSequenceMergedIndex<
            OpportunisticSequenceIndexStorage>::build(
                mps_store,
                target,
                backend);

        auto query_sketch =
            sketch_query_for_index_selection<MinimapSketchData>(
                lazy_query,
                mps_params,
                static_cast<std::size_t>(mps_params.window_size),
                static_cast<std::size_t>(mps_params.kmer_size));
        std::optional<bioseq::indexing::GpuMinimizerIndexCache> gpu_index_cache;
        if (pg_vulkan_seed_lookup_selected(mps_params)) {
            gpu_index_cache.emplace(pg_gpu_index_resident_limit_bytes());
        }
        auto* gpu_index_cache_ptr =
            gpu_index_cache ? &*gpu_index_cache : nullptr;
        const std::uint64_t transient_identity_base =
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(&mps_store));
        bioseq::indexing::MinimizerAssemblyScratch scratch;
        auto run_mps_index =
            [&mps_store, &lazy_query, &target, &query_sketch, &mps_params,
             total_bp_size, &scratch, gpu_index_cache_ptr,
             transient_identity_base, backend](
                    sequence_size_t clip_lo_bp,
                    sequence_size_t clip_hi_bp) {
            auto index =
                bioseq::indexing::MinimizerSequenceMergedIndex<
                    OpportunisticSequenceIndexStorage>::assemble_for_query_segment(
                        mps_store,
                        query_sketch.as_span(),
                        total_bp_size,
                        mps_params.max_occ,
                        clip_lo_bp,
                        clip_hi_bp,
                        scratch);
            const auto gpu_identity =
                pg_gpu_index_identity(
                    transient_identity_base,
                    kMinimizersPerSequenceIndex,
                    clip_lo_bp,
                    clip_hi_bp,
                    backend);
            return minimap2_port_with_index_pg_checked(
                lazy_query,
                target,
                query_sketch.minimizers,
                index,
                mps_params,
                gpu_index_cache_ptr,
                gpu_identity);
        };
        if (total_bp_size <= kPgSegmentationThresholdBp) {
            collection = run_mps_index(0, total_bp_size);
        } else {
            constexpr std::size_t lpu = Target::traits::letters_per_unit;
            MatchResultCollection merged_collection;
            std::vector<MatchResult>& merged = merged_collection.results;
            const sequence_size_t query_len = lazy_query.bp_size();
            for (auto sr : pg_segment_unit_ranges(
                     total_bp_size, query_len, lpu, mps_params)) {
                const sequence_size_t clip_lo_bp =
                    static_cast<sequence_size_t>(sr.cs_u) * lpu;
                const sequence_size_t clip_hi_bp =
                    std::min(
                        static_cast<sequence_size_t>(sr.ce_u) * lpu,
                        total_bp_size);
                MatchResultCollection seg_res =
                    run_mps_index(clip_lo_bp, clip_hi_bp);
                merged_collection.combine_execution_proof(seg_res);
                for (auto& r : seg_res.results) {
                    if (r.target_start >= sr.core_lo_bp
                        && r.target_start < sr.core_hi_bp) {
                        merged.push_back(std::move(r));
                    }
                }
            }
            collection = std::move(merged_collection);
        }
        const bool exact_simd_query_selected =
            mps_params.minimizer_algorithm
                == MinimizerAlgorithmType::TWO_STACK_NTHASH
            && mps_params.chaining_algorithm
                == ChainingAlgorithmType::SIMD_MM2
            && mps_params.base_alignment_algorithm
                == BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        if (exact_simd_query_selected) {
            if (!query_sketch.exact_simd_execution_proof_present
                || !query_sketch.exact_simd_execution_proof.accepted(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<ERR>(
                    "Persisted-index ExactSimd query lacks actual execution proof");
                std::abort();
            }
            collection.record_exact_simd_query_execution_proof(
                query_sketch.exact_simd_execution_proof);
        } else if (query_sketch.exact_simd_execution_proof_present) {
            bioseq_log<ERR>(
                "Non-ExactSimd persisted-index query emitted ExactSimd proof");
            std::abort();
        }
    return collection;
}

[[nodiscard]] bool stage_recompute_mps_build_if_selected(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        const Minimap2Params& params,
        std::string_view reason) {
    if (!can_defer_recompute_minimizer_persist(ctx, params)) {
        if (ctx.recompute_async_persist_active
            && target_backend_from_params_for_recompute(params)
                == bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
                    PaperNtHashSimd) {
            bioseq_log<DBG>("[bioseq] index: recompute async-mps-build skipped row={} target_bp={} reason={} because Paper persistence requires a scheme-tagged READY generation; Paper recompute continues without fallback",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.total_bp_size),
                    reason);
        } else if (ctx.recompute_async_persist_active
            && (params.kmer_size != 15 || params.window_size != 10)) {
            bioseq_log<DBG>("[bioseq] index: recompute async-mps-build skipped row={} target_bp={} reason={} because active params are k={} w={} but persisted minimizer indexes are pinned to k=15 w=10",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.total_bp_size),
                    reason,
                    params.kmer_size,
                    params.window_size);
        } else if (!ctx.recompute_async_persist_active
                   || !ctx.index_async_persist_active
                   || !OidIsValid(ctx.user_relid)) {
            bioseq_log<DBG>("[bioseq] index: recompute async-mps-build skipped row={} target_bp={} reason={} gates recompute_async={} index_async={} user_relid_valid={}",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.total_bp_size),
                    reason,
                    ctx.recompute_async_persist_active,
                    ctx.index_async_persist_active,
                    OidIsValid(ctx.user_relid));
        }
        return false;
    }

    const auto choice = choose_deferred_minimizer_persist_index(ctx);
    if (!choice.has_value()) {
        bioseq_log<DBG>("[bioseq] index: recompute async-mps-build skipped row={} target_bp={} reason={} because no minimizer index is enabled for UPDATE_ON_INSERT",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.total_bp_size),
                reason);
        return false;
    }
    if (choice->kind != DeferredMinimizerIndexKind::per_sequence) {
        bioseq_log<DBG>("[bioseq] index: recompute async-mps-build skipped row={} index={} target_bp={} reason={} because deferred choice is not per-sequence",
                static_cast<unsigned long long>(sequence_rowno),
                choice->index_name,
                static_cast<unsigned long long>(loc.total_bp_size),
                reason);
        return false;
    }

    const auto persist_target =
        make_fragment_index_persist_target(
            ctx, loc, sequence_rowno, choice->index_name);
    const bool staged = bioseq::parallelism::pg::stage_async_mps_build(
        persist_target,
        loc,
        alphabet_code,
        storage_unit_byte_width,
        last_fragment_padding,
        static_cast<std::int32_t>(
            bioseq::indexing::detail::pair_run_sort_impl()),
        bioseq_scheduler_pipeline_concurrency_mode,
        bioseq_scheduler_max_concurrency,
        bioseq_index_memory_cap_mb,
        static_cast<std::int32_t>(  // DOC0345 Patch K — thread backend into the async worker
            target_backend_from_params_for_recompute(params)));
    bioseq_log<DBG>("[bioseq] index: recompute async-mps-build row={} index={} target_bp={} reason={} async_staged={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice->index_name,
            static_cast<unsigned long long>(loc.total_bp_size),
            reason,
            staged);
    return staged;
}

[[nodiscard]] bool stage_opportunistic_fragment_store_for_async_persist(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        const OpportunisticFragmentIndexStorage& ram_store,
        std::uint64_t sequence_rowno,
        std::string_view index_name = kMinimizersPerFragmentIndex) {
    if (!OidIsValid(ctx.user_relid) || ram_store.chunks.empty()) {
        return false;
    }

    std::vector<bioseq::parallelism::pg::PersistChunk> persist_chunks;
    persist_chunks.reserve(ram_store.chunks.size());
    for (const OpportunisticIndexChunk& chunk : ram_store.chunks) {
        persist_chunks.push_back(
            bioseq::parallelism::pg::PersistChunk{
                .sub_key = chunk.sub_key,
                .chunk_no = chunk.chunk_no,
                .bytes = std::span<const char>(
                    chunk.bytes.data(), chunk.bytes.size()),
            });
    }

    const bioseq::parallelism::pg::PersistTarget persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, index_name);
    return bioseq::parallelism::pg::stage_async_index_persist(
        persist_target,
        std::span<const bioseq::parallelism::pg::PersistChunk>(
            persist_chunks.data(), persist_chunks.size()));
}

template <class LazyQuery, class ResidentTarget>
[[nodiscard]] MatchResultCollection
run_recompute_with_deferred_minimizer_persist(
        LazyQuery& lazy_query,
        ResidentTarget& resident_target,
        const Minimap2Params& params,
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        const DeferredMinimizerPersistChoice& choice) {
    if (choice.kind == DeferredMinimizerIndexKind::per_sequence) {
        // DOC0345 Patch J — REAL mps no_index: build a transient minimizers_per_sequence
        // index (GPU-backed when Vulkan is selected), assemble it for the query, and run
        // the indexed aligner. No mpf relabelling. ctx is unused on this transient path.
        (void)ctx;
        (void)alphabet_code;
        (void)storage_unit_byte_width;
        (void)last_fragment_padding;
        MatchResultCollection collection = build_and_align_transient_mps(
            lazy_query,
            resident_target,
            params,
            static_cast<sequence_size_t>(loc.total_bp_size));

        bioseq_log<DBG>(
            "[bioseq] index: recompute real-mps-noindex row={} index={} "
            "target_bp={} persisted=false backend={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.total_bp_size),
            bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name(
                target_backend_from_params_for_recompute(params)));
        return collection;
    }

	(void)ctx;
	(void)alphabet_code;
	(void)storage_unit_byte_width;
	(void)last_fragment_padding;
	MatchResultCollection collection = minimap_port_segmented_pg_checked(
		lazy_query, resident_target, params);
	bioseq_log<DBG>("[bioseq] index: recompute external-file persist prohibited row={} index={} target_bp={} constructor_entries={} writer_entries={} async_staged=false",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.total_bp_size),
			static_cast<unsigned long long>(
				bioseq::storage::indexing::
					g_prohibited_external_file_producer_entry_counters
					.constructor_entries),
			static_cast<unsigned long long>(
				bioseq::storage::indexing::
					g_prohibited_external_file_producer_entry_counters
					.writer_entries));
    return collection;
}

template <class R>
struct ResidentTargetForWorkers {
    using traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using resident_config = sequence_custom_alphabet<
        typename traits::alphabet_type,
        storage_unit,
        traits::bits_per_char>;
    using resident_range = compact_sequence_range<resident_config>;

    bioseq::parallelism::MemoryPermit permit;
    resident_range range;
};

template <class R, class Scanner>
[[nodiscard]] std::optional<ResidentTargetForWorkers<R>>
try_materialize_target_for_segment_workers(
        const bioseq::storage::lazy_compact_sequence<R, Scanner>& target_seq,
        std::uint64_t sequence_rowno) {
    if constexpr (!Scanner::supports_fragments()) {
        return std::nullopt;
    } else {
        using resident_t = ResidentTargetForWorkers<R>;
        using storage_unit = typename resident_t::storage_unit;

        const std::uint64_t total_units64 =
            static_cast<std::uint64_t>(target_seq.size());
        if (total_units64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            bioseq_log<ERR, PG>(
                   "bioseq resident target: target row {} has too many packed "
                   "units to materialize in one process",
                   static_cast<unsigned long long>(sequence_rowno));
        }
        const std::size_t total_units =
            static_cast<std::size_t>(total_units64);
        const std::uint64_t packed_bytes =
            bioseq::parallelism::saturating_mul(
                total_units64,
                static_cast<std::uint64_t>(sizeof(storage_unit)));
        constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
        const std::uint64_t reserve_bytes =
            packed_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - context_reserve_bytes
                ? (std::numeric_limits<std::uint64_t>::max)()
                : packed_bytes + context_reserve_bytes;

        auto permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(reserve_bytes);
        if (!permit) {
            bioseq_log<DBG>("[bioseq] resident target: row={} target_bp={} packed_bytes={} "
                    "not admitted; using scanner_pg serial fallback",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(target_seq.bp_size()),
                    static_cast<unsigned long long>(packed_bytes));
            return std::nullopt;
        }

        typename resident_t::resident_range::base_container packed(total_units);
        const std::size_t fragment_units = Scanner::fragment_storage_units;
        for (std::size_t fragment_no = 0;
             fragment_no < target_seq.number_of_fragments();
             ++fragment_no) {
            if ((fragment_no & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }
            const std::span<const storage_unit> fragment =
                target_seq.scanner().read_fragment(fragment_no);
            const std::size_t dst_offset = fragment_no * fragment_units;
            if (dst_offset >= packed.size()) {
                target_seq.scanner().release_fragment(fragment_no);
                break;
            }
            const std::size_t copy_units =
                std::min(fragment.size(), packed.size() - dst_offset);
            std::copy_n(fragment.data(), copy_units, packed.data() + dst_offset);
            target_seq.scanner().release_fragment(fragment_no);
        }

        bioseq_log<DBG>("[bioseq] resident target: row={} target_bp={} fragments={} "
                "packed_bytes={} materialized for segment workers",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(target_seq.bp_size()),
                target_seq.number_of_fragments(),
                static_cast<unsigned long long>(packed_bytes));

        return resident_t{
            std::move(*permit),
            resident_t::resident_range::from_packed(
                std::move(packed), target_seq.padding())};
    }
}

#if !defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_SCANNER_HELPER) \
    && !defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_HELPER_ONLY)
// Keep the PG-backed scanner row path as a separate function so GCC does not
// compile the whole typed scheduler as one very large control-flow graph.
template <class S>
[[nodiscard]] MatchResultCollection process_scanner_pg_row_typed(
    compact_sequence_range<S>& query_seq,
    const pgseq_t* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    const Minimap2Params& dispatch_params,
    const Minimap2Params& params,
    const bool per_row_query_mode,
    std::optional<std::vector<MinimapSketchData>>& cached_query_minimizers,
    std::optional<pipe::MinimizerRangeExecutionProof>&
        cached_query_minimizer_proof,
    bool& consumed_shared_cached_query_proof,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx,
    bioseq::parallelism::GlobalPool& pool)
{
    MatchResultCollection collection;
    // Recompute-owned and per-row sketches are independent operations. A
    // broadcast persisted-query sketch is shared across rows: every consuming
    // job receives its proof for route acceptance, while the caller accounts
    // the shared operation only once on the first proof-valid consumer.
    consumed_shared_cached_query_proof = false;
    bool row_consumed_persisted_query_sketch = false;
	auto* cpu_prepared_target = scanner_pg_ctx->cpu_prepared_target;
	if (cpu_prepared_target != nullptr) {
		if (selected_vulkan_execution(params)) {
			bioseq_report<ERR, PG>()
				.code(ERRCODE_FEATURE_NOT_SUPPORTED)
				.msg("Vulkan execution cannot consume a CPU prepared target");
			__builtin_unreachable();
		}
		if (!cpu_prepared_target->unions_ready
			|| cpu_prepared_target->target_rowno
				!= scanner_pg_ctx->target_rownos[i]
			|| cpu_prepared_target->target_bp_size
				!= static_cast<std::uint64_t>(target_bp_size)) {
			bioseq_report<ERR, PG>()
				.code(ERRCODE_DATA_CORRUPTED)
				.msg("BioseqBatchJoin CPU prepared-target identity is invalid");
			__builtin_unreachable();
		}
	}
    scanner_pg_ctx->target_index_sources[i] =
        ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    std::optional<std::vector<MinimapSketchData>> row_query_minimizers;
    std::optional<pipe::MinimizerRangeExecutionProof>
        row_query_minimizer_proof;
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
    // DOC0264 §9.2 / DOC0274 — production scanner_pg path.
    using SP = bioseq::storage::sequence_scanner_pg<S>;
    bioseq::storage::lazy_compact_sequence<S, SP> fq_pg{
        bioseq::storage::make_scanner_pg_in_memory<S>(query_seq)};
    const auto& loc_opt = scanner_pg_ctx->target_locators[i];
    const bool persisted_required =
        scanner_pg_ctx->index_read_requirement
        == bioseq::indexing::IndexReadRequirement::
            require_compatible_persisted;
    if (persisted_required) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[i],
            "no current MPF/MPS reader validates a revision-3 READY "
            "identity; pair-sort rebuild and legacy cache reuse are forbidden");
    }
    if (persisted_required && !loc_opt.has_value()) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[i],
            "target payload is inline; persisted minimizer storage requires "
            "a locator-backed target");
    }
    if (persisted_required && scanner_pg_ctx->target_rownos[i] < 0) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[i],
            "logical target row identity is unavailable");
    }
    if (loc_opt.has_value()) {
        // DOC0274 — compute last-fragment padding from
        // total_bp_size and S's letters_per_unit. For
        // total_bp = N, padding = ceil(N/lpu)*lpu - N.
        constexpr std::size_t _lpu =
            compact_sequence_range<S>::traits::letters_per_unit;
        const std::uint64_t _total_units =
            (loc_opt->total_bp_size + _lpu - 1) / _lpu;
        const std::size_t _padding =
            static_cast<std::size_t>(
                _total_units * _lpu - loc_opt->total_bp_size);
        SP target_scanner(
            scanner_pg_ctx->db_oid,
            scanner_pg_ctx->tablespace_oid,
            scanner_pg_ctx->main_relfilenode,
            scanner_pg_ctx->mmsearch_seq_attno,
            *loc_opt,
            _padding,
            scanner_pg_ctx->snapshot,
            scanner_pg_ctx->caches,
            scanner_pg_ctx->parent_mctx);
        bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
            std::move(target_scanner)};
        // DOC0279 §11.3a — ASSEMBLE from the stored
        // per-fragment minimizer index if present
        // (deserialise + globalise, no re-collection);
        // else fall back to recompute (DOC0276).
        bool assembled = false;
        /*
         * Persisted-payload reads and opportunistic recompute persistence are
         * independent policies.  In particular, require_absent deliberately
         * clears read_index_active, but recompute_async_persist must still be
         * able to build and enqueue the default MPF index from the source
         * sequence.  Keep the existing MPF RAM-first path reachable when both
         * async write gates are captured for this scan; every prepared,
         * sidecar, and assembled-cache read below remains guarded by
         * read_index_active.
         */
        const auto requested_target_backend =
            target_backend_from_params_for_recompute(dispatch_params);
        const bool legacy_minimizer_scheme_compatible =
            requested_target_backend
            != bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
                PaperNtHashSimd;
        if (scanner_pg_ctx->read_index_active
            && !legacy_minimizer_scheme_compatible) {
            bioseq_log<DBG>(
                "[bioseq] index: legacy persisted read skipped row={} "
                "backend=paper_nthash_simd reason=payload_has_no_scheme_identity",
                static_cast<unsigned long long>(
                    scanner_pg_ctx->target_rownos[i]));
        }
        const bool opportunistic_mpf_recompute_requested =
            !scanner_pg_ctx->read_index_active
            && scanner_pg_ctx->recompute_async_persist_active
            && scanner_pg_ctx->index_async_persist_active
            && legacy_minimizer_scheme_compatible
            && scanner_pg_ctx->active_index
                == kMinimizersPerFragmentIndex;
        if (((scanner_pg_ctx->read_index_active
                    && legacy_minimizer_scheme_compatible)
                || opportunistic_mpf_recompute_requested)
            && OidIsValid(scanner_pg_ctx->user_relid)) {
            using IStore = bioseq::storage::indexing::IndexStorageFragmentPg;
            const bool want_seq_recompute =
                scanner_pg_ctx->active_index == kMinimizersPerSequenceIndex;
            const bool want_seq = want_seq_recompute;
            // The stored index is ALWAYS w=10,k=15 (pinned at build); the query
            // minimizers + the seed pipeline MUST use the same w,k (NOT the size
            // heuristic's reduced k), exactly as the legacy prebuilt branch does.
            Minimap2Params idx_params = dispatch_params;
            idx_params.kmer_size = 15; idx_params.window_size = 10;
            if (min_bp < 50) { idx_params.min_chain_score = 10; idx_params.min_seed_count = 1; }
            else if (min_bp < 200) { idx_params.min_chain_score = 20; idx_params.min_seed_count = 2; }
            std::optional<bioseq::indexing::GpuMinimizerIndexCache>
                pg_gpu_index_cache;
            if (pg_vulkan_seed_lookup_selected(idx_params)) {
                pg_gpu_index_cache.emplace(
                    pg_gpu_index_resident_limit_bytes());
            }
            auto* pg_gpu_index_cache_ptr =
                pg_gpu_index_cache ? &*pg_gpu_index_cache : nullptr;
            const std::uint64_t target_rowno =
                static_cast<std::uint64_t>(
                    scanner_pg_ctx->target_rownos[i]);
            auto& query_minimizers =
                per_row_query_mode
                    ? row_query_minimizers
                    : cached_query_minimizers;
            auto& query_minimizer_proof =
                per_row_query_mode
                    ? row_query_minimizer_proof
                    : cached_query_minimizer_proof;
            auto ensure_q =
                [&query_minimizers, &query_minimizer_proof,
                 &query_seq, &idx_params,
                 &row_consumed_persisted_query_sketch] {
                if (!query_minimizers.has_value()) {
                    if (selected_vulkan_execution(idx_params)) {
                        bioseq::gpu::vulkan::VulkanRuntimeError err{
                            "persisted Vulkan query sketch cannot use the CPU prepared-index path"};
                        raise_vulkan_pg_error(
                            "persisted query minimizer sketch", err.what());
                    }
                    sketch_collector qc;
                    const MinimizerSketchBackend backend =
                        pipe::target_index_sketch_backend_from_params(
                            idx_params);
                    auto proof = pipe::minimizer_dispatch_range<
                        MinimapSketchData>(
                        qc,
                        query_seq,
                        0,
                        static_cast<sequence_size_t>(query_seq.size()),
                        static_cast<size_t>(idx_params.window_size),
                        static_cast<size_t>(idx_params.kmer_size),
                        backend);
                    if (!pipe::minimizer_range_proof_matches_backend(
                            backend,
                            proof,
                            /*require_nonempty_vector_work=*/false)) {
                        bioseq_report<ERR, PG>()
                            .code(ERRCODE_INTERNAL_ERROR)
                            .msg("persisted query sketch proof does not match the selected minimizer backend");
                        __builtin_unreachable();
                    }
                    query_minimizers.emplace(std::move(qc).extract_sketches_data());
                    query_minimizer_proof.emplace(std::move(proof));
                } else if (!query_minimizer_proof.has_value()) {
                    bioseq_report<ERR, PG>()
                        .code(ERRCODE_DATA_CORRUPTED)
                        .msg("cached persisted query minimizers have no operation proof");
                    __builtin_unreachable();
                }
                row_consumed_persisted_query_sketch = true;
            };
			auto* prepared_group = cpu_prepared_target == nullptr
				? nullptr
				: cpu_prepared_target->find_group(
					idx_params.kmer_size,
					idx_params.window_size,
					idx_params.max_occ,
					pipe::target_index_sketch_backend_from_params(idx_params));
			if (cpu_prepared_target != nullptr && prepared_group == nullptr) {
				bioseq_report<ERR, PG>()
					.code(ERRCODE_DATA_CORRUPTED)
					.msg("BioseqBatchJoin prepared query group is missing");
				__builtin_unreachable();
			}
			if (scanner_pg_ctx->read_index_active
				&& prepared_group != nullptr
				&& prepared_group->target_index) {
				ensure_q();
				collection = minimap2_port_with_index_pg_checked(
					fq_pg,
					ft_pg,
					*query_minimizers,
					*prepared_group->target_index,
					idx_params,
					/*gpu_index_cache=*/nullptr,
					bioseq::indexing::GpuIndexCacheIdentity{
						.assembly_identity = prepared_group->key_set_checksum});
				++prepared_group->target_index_reuses;
				assembled = true;
				scanner_pg_ctx->target_index_sources[i] =
					prepared_group->target_index_source;
			}
	                                auto make_cache_key =
	                                    [scanner_pg_ctx, loc_opt, &query_minimizers, &idx_params, i](
	                                            std::string_view index_name,
	                                            sequence_size_t clip_lo_bp,
	                                            sequence_size_t clip_hi_bp) {
                const auto query_span = std::span<const MinimapSketchData>(
                    query_minimizers->data(), query_minimizers->size());
                const auto sketch_backend =
                    pipe::target_index_sketch_backend_from_params(idx_params);
                return bioseq::storage::indexing::AssembledIndexKey{
                    .db_oid = scanner_pg_ctx->db_oid,
                    .user_relid = scanner_pg_ctx->user_relid,
                    .tablespace_oid = scanner_pg_ctx->tablespace_oid,
                    .main_relfilenode = scanner_pg_ctx->main_relfilenode,
                    .column_attno = scanner_pg_ctx->mmsearch_seq_attno,
                    .index_name_hash = bioseq::storage::indexing::index_name_hash(
                        scanner_pg_ctx->db_oid,
                        scanner_pg_ctx->tablespace_oid,
                        scanner_pg_ctx->main_relfilenode,
                        scanner_pg_ctx->mmsearch_seq_attno,
                        index_name),
                    .sketch_backend = sketch_backend,
                    .minimizer_scheme =
                        bioseq::indexing::minimizer_scheme_identity_for_backend(
                            sketch_backend),
                    .query_key = static_cast<std::uint64_t>(
                        scanner_pg_ctx->target_rownos[i]),
                    .query_signature =
                        bioseq::storage::indexing::query_minimizer_signature(
                            query_span,
                            static_cast<std::uint32_t>(idx_params.kmer_size),
                            static_cast<std::uint32_t>(idx_params.window_size),
                            static_cast<std::uint32_t>(idx_params.max_occ)),
                    .clip_lo_bp = clip_lo_bp,
	                                        .clip_hi_bp = clip_hi_bp,
	                                    };
	                                };
		                                    auto run_fragment_backed_index =
		                                    [&ensure_q, &loc_opt, &idx_params,
                         &query_seq, &query_minimizers,
                         &collection, &assembled,
                         &make_cache_key, &fq_pg, &ft_pg,
		                 pg_gpu_index_cache_ptr, prepared_group,
		                 cpu_prepared_target](
                                IStore& fragment_store,
		                                            std::string_view cache_index_name) {
	                                    ensure_q();
	                                    const sequence_size_t total_bp =
	                                        loc_opt->total_bp_size;
	                                    const std::size_t frag_units =
	                                        SP::fragment_storage_units;
	                                    const std::size_t a_lpu =
	                                        compact_sequence_range<S>::traits::letters_per_unit;
	                                    const sequence_size_t frag_bp =
	                                        static_cast<sequence_size_t>(frag_units) * a_lpu;
	                                    const std::size_t size_u =
	                                        static_cast<std::size_t>(
	                                            (total_bp + a_lpu - 1) / a_lpu);
	                                    bioseq::indexing::MinimizerAssemblyScratch scratch;
	                                    const auto& assembly_minimizers =
	                                        prepared_group == nullptr
	                                            ? *query_minimizers
	                                            : prepared_group->query_hash_union;
	                                    auto assemble_one =
	                                        [frag_units, a_lpu, total_bp, frag_bp,
	                                         &fragment_store, &scratch,
	                                         &assembly_minimizers](
	                                                std::size_t cs_u,
	                                                std::size_t ce_u) {
	                                        const std::size_t frag_lo = cs_u / frag_units;
	                                        const std::size_t frag_hi =
	                                            (ce_u + frag_units - 1) / frag_units;
	                                        const sequence_size_t cs_bp =
	                                            static_cast<sequence_size_t>(cs_u) * a_lpu;
	                                        const sequence_size_t ce_bp =
	                                            std::min(
	                                                static_cast<sequence_size_t>(ce_u) * a_lpu,
	                                                total_bp);
	                                        const auto query_span =
	                                            std::span<const MinimapSketchData>(
	                                                assembly_minimizers.data(),
	                                                assembly_minimizers.size());
	                                        return bioseq::indexing::MinimizersPerFragmentIndex<
	                                            IStore>::assemble_segment_for_query(
	                                                fragment_store,
	                                                query_span,
	                                                frag_lo,
	                                                frag_hi,
	                                                frag_bp,
	                                                total_bp,
	                                                cs_bp,
	                                                ce_bp,
	                                                scratch);
	                                    };
	                                    auto run_with_segment_index =
	                                        [&make_cache_key, &assemble_one, &fq_pg, &ft_pg,
	                                         &query_minimizers, &idx_params, cache_index_name,
	                                         pg_gpu_index_cache_ptr,
	                                         prepared_group,
	                                         cpu_prepared_target,
	                                         total_bp](
	                                                sequence_size_t clip_lo_bp,
	                                                sequence_size_t clip_hi_bp,
	                                                std::size_t cs_u,
	                                                std::size_t ce_u) {
	                                        const auto cache_key = make_cache_key(
	                                            cache_index_name, clip_lo_bp, clip_hi_bp);
	                                        const auto gpu_identity =
	                                            pg_gpu_index_identity(cache_key);
	                                        if (prepared_group == nullptr) if (auto hit =
	                                                bioseq::storage::indexing::lookup_assembled_index(
	                                                    cache_key)) {
	                                            return minimap2_port_with_index_pg_checked(
	                                                fq_pg,
	                                                ft_pg,
	                                                *query_minimizers,
	                                                *hit->index,
	                                                idx_params,
	                                                pg_gpu_index_cache_ptr,
	                                                gpu_identity);
	                                        }
	                                        if (prepared_group != nullptr
	                                            && !prepared_group->target_index_permit.has_value()) {
	                                            const std::uint64_t admission_bytes =
	                                                bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
	                                                    static_cast<std::uint64_t>(total_bp));
	                                            auto permit =
	                                                bioseq::parallelism::MemoryAdmissionController::instance()
	                                                    .try_reserve(admission_bytes);
	                                            if (!permit) {
	                                                bioseq_report<ERR, PG>()
	                                                    .code(ERRCODE_OUT_OF_MEMORY)
	                                                    .msg("BioseqBatchJoin prepared persisted target index cannot be admitted");
	                                                __builtin_unreachable();
	                                            }
	                                            prepared_group->target_index_permit.emplace(
	                                                std::move(*permit));
	                                        }
	                                        auto idx = assemble_one(cs_u, ce_u);
	                                        if (prepared_group != nullptr) {
	                                            if (prepared_group->target_index) {
	                                                ++prepared_group->duplicate_build_attempts;
	                                            } else {
	                                                prepared_group->target_index =
	                                                    std::make_shared<
	                                                        bioseq::indexing::MinimizerHashmapFragmentIndex>(
	                                                            std::move(idx));
	                                                prepared_group->target_index_source =
	                                                    ::bioseq::pipeline_stats::TargetIndexSource::persisted;
	                                                ++prepared_group->target_index_builds;
	                                                prepared_group->build_set_checksum_present = true;
	                                                prepared_group->build_set_checksum =
	                                                    prepared_group->key_set_checksum;
	                                                ++cpu_prepared_target->physical_target_index_reads;
	                                            }
	                                            return minimap2_port_with_index_pg_checked(
	                                                fq_pg,
	                                                ft_pg,
	                                                *query_minimizers,
	                                                *prepared_group->target_index,
	                                                idx_params,
	                                                /*gpu_index_cache=*/nullptr,
	                                                bioseq::indexing::GpuIndexCacheIdentity{
	                                                    .assembly_identity =
	                                                        prepared_group->key_set_checksum});
	                                        }
	                                        MatchResultCollection out =
	                                            minimap2_port_with_index_pg_checked(
	                                                fq_pg,
	                                                ft_pg,
	                                                *query_minimizers,
	                                                idx,
	                                                idx_params,
	                                                pg_gpu_index_cache_ptr,
	                                                gpu_identity);
	                                        bioseq::storage::indexing::store_assembled_index(
	                                            cache_key, std::move(idx));
	                                        return out;
	                                    };
	                                    if (prepared_group != nullptr) {
	                                        /* One full-target union-filtered index is the
	                                         * scan-retained object reused by every wave. */
	                                        collection = run_with_segment_index(
	                                            0, total_bp, 0, size_u);
	                                    } else if (total_bp <= kPgSegmentationThresholdBp) {
	                                        collection = run_with_segment_index(
	                                            0, total_bp, 0, size_u);
	                                    } else {
	                                        MatchResultCollection merged_collection;
	                                        std::vector<MatchResult>& merged =
	                                            merged_collection.results;
	                                        const sequence_size_t query_len =
	                                            query_seq.bp_size();
	                                        for (auto sr : pg_segment_unit_ranges(
	                                                 total_bp,
	                                                 query_len,
	                                                 a_lpu,
	                                                 idx_params)) {
	                                            const sequence_size_t clip_lo_bp =
	                                                static_cast<sequence_size_t>(sr.cs_u) * a_lpu;
	                                            const sequence_size_t clip_hi_bp =
	                                                std::min(
	                                                    static_cast<sequence_size_t>(sr.ce_u) * a_lpu,
	                                                    total_bp);
	                                            MatchResultCollection seg_res =
	                                                run_with_segment_index(
	                                                    clip_lo_bp,
	                                                    clip_hi_bp,
	                                                    sr.cs_u,
	                                                    sr.ce_u);
	                                            merged_collection.combine_execution_proof(
	                                                seg_res);
	                                            for (auto& r : seg_res.results) {
	                                                if (r.target_start >= sr.core_lo_bp
	                                                    && r.target_start < sr.core_hi_bp) {
	                                                    merged.push_back(std::move(r));
	                                                }
	                                            }
	                                        }
	                                        collection = std::move(merged_collection);
	                                    }
	                                    assembled = true;
	                                };
	                                // DOC0279 §11.3 — route by the table's ACTIVE read index
	                                // (set_active_read_index / resolve_active_read_index, default
	                                // minimizers_per_fragment). Per-row presence still gates: an
            // active index not materialised for THIS row recomputes.
		                                if (scanner_pg_ctx->read_index_active
		                                    && !assembled) {
	                                if (want_seq) {
	                                    auto seq_store =
	                                        bioseq::storage::indexing::open_fragment_storage(
	                                            scanner_pg_ctx->user_relid,
	                                            scanner_pg_ctx->db_oid,
	                                            scanner_pg_ctx->tablespace_oid,
	                                            scanner_pg_ctx->main_relfilenode,
	                                            scanner_pg_ctx->mmsearch_seq_attno,
	                                            kMinimizersPerSequenceIndex,
	                                            *loc_opt,
	                                            static_cast<std::uint64_t>(
	                                                scanner_pg_ctx->target_rownos[i]),
	                                            scanner_pg_ctx->caches,
	                                            scanner_pg_ctx->parent_mctx);
	                                    if (seq_store.number_of_chunks(
	                                            bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
	                                        const std::size_t dir_bytes =
	                                            seq_store.chunk_byte_size(
	                                                bioseq::indexing::DIRECTORY_SUB_KEY,
	                                                0);
	                                        if (dir_bytes == sizeof(std::int64_t)) {
	                                            run_fragment_backed_index(
	                                                seq_store,
	                                                kMinimizersPerSequenceIndex);
	                                            bioseq_log<DBG>("[bioseq] index: logical fragment-backed read index=minimizers_per_sequence row={} target_bp={}",
	                                                    static_cast<unsigned long long>(
	                                                        scanner_pg_ctx->target_rownos[i]),
	                                                    static_cast<unsigned long long>(
	                                                        loc_opt->total_bp_size));
	                                        } else {
	                                            ensure_q();
	                                            const sequence_size_t total_bp =
	                                                loc_opt->total_bp_size;
	                                            constexpr std::size_t a_lpu =
	                                                compact_sequence_range<S>::traits::letters_per_unit;
	                                            const std::vector<MinimapSketchData>& query_vec =
	                                                *query_minimizers;
	                                            const std::vector<MinimapSketchData>& assembly_query_vec =
	                                                prepared_group == nullptr
	                                                    ? query_vec
	                                                    : prepared_group->query_hash_union;
	                                            const auto query_span =
	                                                std::span<const MinimapSketchData>(
	                                                    assembly_query_vec.data(),
	                                                    assembly_query_vec.size());
	                                            bioseq::indexing::MinimizerAssemblyScratch sq_scratch;
	                                            auto run_mps_index =
	                                                [&make_cache_key, &seq_store, &fq_pg, &ft_pg,
	                                                 &idx_params, &query_vec, query_span, total_bp,
	                                                 &sq_scratch, pg_gpu_index_cache_ptr,
	                                                 prepared_group,
	                                                 cpu_prepared_target](
	                                                        sequence_size_t clip_lo_bp,
	                                                        sequence_size_t clip_hi_bp) {
	                                                const auto cache_key = make_cache_key(
	                                                    kMinimizersPerSequenceIndex,
	                                                    clip_lo_bp,
	                                                    clip_hi_bp);
	                                                const auto gpu_identity =
	                                                    pg_gpu_index_identity(cache_key);
	                                                if (prepared_group == nullptr) if (auto hit =
	                                                        bioseq::storage::indexing::lookup_assembled_index(
	                                                            cache_key)) {
	                                                    return minimap2_port_with_index_pg_checked(
	                                                        fq_pg,
	                                                        ft_pg,
	                                                        query_vec,
	                                                        *hit->index,
	                                                        idx_params,
	                                                        pg_gpu_index_cache_ptr,
	                                                        gpu_identity);
	                                                }
	                                                if (prepared_group != nullptr
	                                                    && !prepared_group->target_index_permit.has_value()) {
	                                                    const std::uint64_t admission_bytes =
	                                                        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
	                                                            static_cast<std::uint64_t>(total_bp));
	                                                    auto permit =
	                                                        bioseq::parallelism::MemoryAdmissionController::instance()
	                                                            .try_reserve(admission_bytes);
	                                                    if (!permit) {
	                                                        bioseq_report<ERR, PG>()
	                                                            .code(ERRCODE_OUT_OF_MEMORY)
	                                                            .msg("BioseqBatchJoin prepared persisted target index cannot be admitted");
	                                                        __builtin_unreachable();
	                                                    }
	                                                    prepared_group->target_index_permit.emplace(
	                                                        std::move(*permit));
	                                                }
	                                                auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<
	                                                    IStore>::assemble_for_query_segment(
	                                                        seq_store,
	                                                        query_span,
	                                                        total_bp,
	                                                        idx_params.max_occ,
	                                                        clip_lo_bp,
	                                                        clip_hi_bp,
	                                                        sq_scratch);
	                                                if (prepared_group != nullptr) {
	                                                    if (prepared_group->target_index) {
	                                                        ++prepared_group->duplicate_build_attempts;
	                                                    } else {
	                                                        prepared_group->target_index =
	                                                            std::make_shared<
	                                                                bioseq::indexing::MinimizerHashmapFragmentIndex>(
	                                                                    std::move(idx));
	                                                        prepared_group->target_index_source =
	                                                            ::bioseq::pipeline_stats::TargetIndexSource::persisted;
	                                                        ++prepared_group->target_index_builds;
	                                                        prepared_group->build_set_checksum_present = true;
	                                                        prepared_group->build_set_checksum =
	                                                            prepared_group->key_set_checksum;
	                                                        ++cpu_prepared_target->physical_target_index_reads;
	                                                    }
	                                                    return minimap2_port_with_index_pg_checked(
	                                                        fq_pg,
	                                                        ft_pg,
	                                                        query_vec,
	                                                        *prepared_group->target_index,
	                                                        idx_params,
	                                                        /*gpu_index_cache=*/nullptr,
	                                                        bioseq::indexing::GpuIndexCacheIdentity{
	                                                            .assembly_identity =
	                                                                prepared_group->key_set_checksum});
	                                                }
	                                                MatchResultCollection out =
	                                                    minimap2_port_with_index_pg_checked(
	                                                        fq_pg,
	                                                        ft_pg,
	                                                        query_vec,
	                                                        idx,
	                                                        idx_params,
	                                                        pg_gpu_index_cache_ptr,
	                                                        gpu_identity);
	                                                bioseq::storage::indexing::store_assembled_index(
	                                                    cache_key,
	                                                    std::move(idx));
	                                                return out;
	                                            };
	                                            if (prepared_group != nullptr) {
	                                                collection = run_mps_index(0, total_bp);
	                                            } else if (total_bp <= kPgSegmentationThresholdBp) {
	                                                collection = run_mps_index(0, total_bp);
	                                            } else {
	                                                MatchResultCollection merged_collection;
	                                                std::vector<MatchResult>& merged =
	                                                    merged_collection.results;
	                                                const sequence_size_t query_len =
	                                                    query_seq.bp_size();
	                                                for (auto sr : pg_segment_unit_ranges(
	                                                         total_bp,
	                                                         query_len,
	                                                         a_lpu,
	                                                         idx_params)) {
	                                                    const sequence_size_t clip_lo_bp =
	                                                        static_cast<sequence_size_t>(sr.cs_u) * a_lpu;
	                                                    const sequence_size_t clip_hi_bp =
	                                                        std::min(
	                                                            static_cast<sequence_size_t>(sr.ce_u) * a_lpu,
	                                                            total_bp);
	                                                    MatchResultCollection seg_res =
	                                                        run_mps_index(clip_lo_bp, clip_hi_bp);
	                                                    merged_collection.combine_execution_proof(
	                                                        seg_res);
	                                                    for (auto& r : seg_res.results) {
	                                                        if (r.target_start >= sr.core_lo_bp
	                                                            && r.target_start < sr.core_hi_bp) {
	                                                            merged.push_back(std::move(r));
	                                                        }
	                                                    }
	                                                }
	                                                collection = std::move(merged_collection);
	                                            }
	                                            assembled = true;
	                                        }
	                                    }
	                                } else {
	                                    auto frag_store =
	                                        bioseq::storage::indexing::open_fragment_storage(
	                                            scanner_pg_ctx->user_relid,
	                                            scanner_pg_ctx->db_oid,
	                                            scanner_pg_ctx->tablespace_oid,
	                                            scanner_pg_ctx->main_relfilenode,
	                                            scanner_pg_ctx->mmsearch_seq_attno,
	                                            kMinimizersPerFragmentIndex,
	                                            *loc_opt,
	                                            static_cast<std::uint64_t>(
	                                                scanner_pg_ctx->target_rownos[i]),
	                                            scanner_pg_ctx->caches,
	                                            scanner_pg_ctx->parent_mctx);
	                                    if (frag_store.number_of_chunks(
	                                            bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
	                                        run_fragment_backed_index(
	                                            frag_store,
	                                            kMinimizersPerFragmentIndex);
	                                    }
	                                }
	                                }
            if (assembled) {
                scanner_pg_ctx->target_index_sources[i] =
                    ::bioseq::pipeline_stats::TargetIndexSource::persisted;
            }
            if (persisted_required && !assembled) {
                raise_required_persisted_index_error(
                    scanner_pg_ctx,
                    scanner_pg_ctx->target_rownos[i],
                    "the active persisted minimizer index has no compatible "
                    "directory/chunks for this target row");
            }
            // DOC0345 §1.4 / acceptance criterion #4 — a recompute under an
            // ACTIVE per-sequence read index with no persisted mps row for
            // this target must build a REAL transient mps (GPU sorted-pair
            // seam, streaming), NOT the opportunistic RAM-first mpf below
            // relabelled as mps. DOC0347's Round-1 fix only covered the
            // async/deferred fast path; this closes the read-index recompute
            // path too (codex review 2026-06-22, dispatch_processor relabel).
            if (!assembled && want_seq) {
                collection = build_and_align_transient_mps(
                    fq_pg,
                    ft_pg,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->total_bp_size));
                assembled = true;
                bioseq_log<DBG>("[bioseq] index: recompute real-mps-readindex row={} target_bp={} backend={}",
                        static_cast<unsigned long long>(
                            scanner_pg_ctx->target_rownos[i]),
                        static_cast<unsigned long long>(
                            loc_opt->total_bp_size),
                        bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name(
                            target_backend_from_params_for_recompute(params)));
            }
	                                if (!assembled
	                                    && loc_opt->total_bp_size >= kOpportunisticIndexMinBp) {
                const std::uint64_t peak_bytes =
                    bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                        static_cast<std::uint64_t>(loc_opt->total_bp_size));
                auto build_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(peak_bytes);
                if (build_permit) {
                    ensure_q();
                    OpportunisticFragmentIndexStorage ram_store;
	                                        bioseq::indexing::MinimizersPerFragmentIndex<
	                                            OpportunisticFragmentIndexStorage>::build(
	                                                ram_store, ft_pg,
                            target_backend_from_params_for_recompute(params));
	                                        const std::string_view opportunistic_index_name =
	                                            want_seq
	                                                ? kMinimizersPerSequenceIndex
	                                                : kMinimizersPerFragmentIndex;

	                                        const bool staged =
	                                            stage_opportunistic_fragment_store_for_async_persist(
	                                                *scanner_pg_ctx,
	                                                *loc_opt,
	                                                ram_store,
	                                                static_cast<std::uint64_t>(
	                                                    scanner_pg_ctx->target_rownos[i]),
	                                                opportunistic_index_name);
	                                        bioseq_log<DBG>("[bioseq] index: opportunistic RAM-first index row={} index={} target_bp={} chunks={} peak_bytes={} async_staged={}",
	                                                static_cast<unsigned long long>(
	                                                    scanner_pg_ctx->target_rownos[i]),
	                                                opportunistic_index_name,
	                                                static_cast<unsigned long long>(
	                                                    loc_opt->total_bp_size),
	                                                ram_store.chunks.size(),
                            static_cast<unsigned long long>(peak_bytes),
                            staged);

                    const sequence_size_t total_bp = loc_opt->total_bp_size;
                    const std::size_t frag_units = SP::fragment_storage_units;
                    const std::size_t a_lpu =
                        compact_sequence_range<S>::traits::letters_per_unit;
                    const sequence_size_t frag_bp =
                        static_cast<sequence_size_t>(frag_units) * a_lpu;
                    const std::size_t size_u =
                        static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                    bioseq::indexing::MinimizerAssemblyScratch scratch;
                    auto assemble_ram_one =
                        [frag_units, a_lpu, total_bp, frag_bp,
                         &ram_store, &scratch, &query_minimizers](
                                std::size_t cs_u,
                                std::size_t ce_u) {
                        const std::size_t frag_lo = cs_u / frag_units;
                        const std::size_t frag_hi =
                            (ce_u + frag_units - 1) / frag_units;
                        const sequence_size_t cs_bp =
                            static_cast<sequence_size_t>(cs_u) * a_lpu;
                        const sequence_size_t ce_bp =
                            std::min(
                                static_cast<sequence_size_t>(ce_u) * a_lpu,
                                total_bp);
                        const auto query_span =
                            std::span<const MinimapSketchData>(
                                query_minimizers->data(),
                                query_minimizers->size());
                        return bioseq::indexing::MinimizersPerFragmentIndex<
                            OpportunisticFragmentIndexStorage>::assemble_segment_for_query(
                                ram_store,
                                query_span,
                                frag_lo,
                                frag_hi,
                                frag_bp,
                                total_bp,
                                cs_bp,
                                ce_bp,
                                scratch);
                    };
                    const bool assembled_cache_allowed =
                        scanner_pg_ctx->read_index_active
                        && scanner_pg_ctx->index_read_requirement
                            != bioseq::indexing::IndexReadRequirement::
                                require_absent;
                    auto run_with_ram_index =
                        [&make_cache_key, &assemble_ram_one, &fq_pg, &ft_pg,
                         &query_minimizers, &idx_params,
                         opportunistic_index_name,
                         pg_gpu_index_cache_ptr,
                         assembled_cache_allowed](
                                sequence_size_t clip_lo_bp,
                                sequence_size_t clip_hi_bp,
                                std::size_t cs_u,
                                std::size_t ce_u) {
                        const auto cache_key = make_cache_key(
                            opportunistic_index_name,
                            clip_lo_bp,
                            clip_hi_bp);
                        const auto gpu_identity =
                            pg_gpu_index_identity(cache_key);
                        if (assembled_cache_allowed) {
                            if (auto hit =
                                bioseq::storage::indexing::lookup_assembled_index(
                                    cache_key)) {
                                return minimap2_port_with_index_pg_checked(
                                    fq_pg,
                                    ft_pg,
                                    *query_minimizers,
                                    *hit->index,
                                    idx_params,
                                    pg_gpu_index_cache_ptr,
                                    gpu_identity);
                            }
                        }
                        auto idx = assemble_ram_one(cs_u, ce_u);
                        MatchResultCollection out =
                            minimap2_port_with_index_pg_checked(
                                fq_pg,
                                ft_pg,
                                *query_minimizers,
                                idx,
                                idx_params,
                                pg_gpu_index_cache_ptr,
                                gpu_identity);
                        if (assembled_cache_allowed) {
                            bioseq::storage::indexing::store_assembled_index(
                                cache_key, std::move(idx));
                        }
                        return out;
                    };
                    if (total_bp <= kPgSegmentationThresholdBp) {
                        collection = run_with_ram_index(
                            0, total_bp, 0, size_u);
                    } else {
                        MatchResultCollection merged_collection;
                        std::vector<MatchResult>& merged =
                            merged_collection.results;
                        const sequence_size_t query_len = query_seq.bp_size();
                        for (auto sr : pg_segment_unit_ranges(
                                 total_bp,
                                 query_len,
                                 a_lpu,
                                 idx_params)) {
                            const sequence_size_t clip_lo_bp =
                                static_cast<sequence_size_t>(sr.cs_u) * a_lpu;
                            const sequence_size_t clip_hi_bp =
                                std::min(
                                    static_cast<sequence_size_t>(sr.ce_u) * a_lpu,
                                    total_bp);
                            MatchResultCollection seg_res =
                                run_with_ram_index(
                                    clip_lo_bp,
                                    clip_hi_bp,
                                    sr.cs_u,
                                    sr.ce_u);
                            merged_collection.combine_execution_proof(seg_res);
                            for (auto& r : seg_res.results) {
                                if (r.target_start >= sr.core_lo_bp
                                    && r.target_start < sr.core_hi_bp) {
                                    merged.push_back(std::move(r));
                                }
                            }
                        }
                        collection = std::move(merged_collection);
                    }
                    assembled = true;
                } else {
                    bioseq_log<DBG>("[bioseq] index: opportunistic RAM-first index skipped row={} target_bp={} peak_bytes={} (not admitted)",
                            static_cast<unsigned long long>(
                                scanner_pg_ctx->target_rownos[i]),
                            static_cast<unsigned long long>(
                                loc_opt->total_bp_size),
                            static_cast<unsigned long long>(peak_bytes));
                }
            }
        }
        // Paper may recompute normally in allow/no-index mode, but it may not
        // manufacture or consume the scheme-less legacy MPS representation.
        // Keep the MPS-shaped transient builder for compatible legacy schemes;
        // Paper falls through to the ordinary Paper pipeline below.
        const bool want_seq_noindex =
            scanner_pg_ctx->active_index == kMinimizersPerSequenceIndex
            && legacy_minimizer_scheme_compatible;
        if (!assembled && want_seq_noindex) {
            const auto rowno =
                static_cast<std::uint64_t>(
                    scanner_pg_ctx->target_rownos[i]);
            // No-index MPS must use a real transient
            // minimizers_per_sequence index even when
            // recompute_async_persist is off. Reads are
            // disabled here, but the active policy still
            // selects the algorithm shape.
            collection = build_and_align_transient_mps(
                fq_pg,
                ft_pg,
                params,
                static_cast<sequence_size_t>(
                    loc_opt->total_bp_size));
            bioseq_log<DBG>("[bioseq] index: recompute real-mps-noindex-policy row={} target_bp={} persisted=false backend={}",
                    static_cast<unsigned long long>(rowno),
                    static_cast<unsigned long long>(
                        loc_opt->total_bp_size),
                    bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name(
                        target_backend_from_params_for_recompute(params)));
            assembled = true;
        }
        if (!assembled) {
            const bool can_defer_fast_mps_recompute =
                can_defer_recompute_minimizer_persist(
                    *scanner_pg_ctx, params);
            const auto fast_mps_choice =
                can_defer_fast_mps_recompute
                    ? choose_deferred_minimizer_persist_index(
                        *scanner_pg_ctx)
                    : std::optional<DeferredMinimizerPersistChoice>{};
            if (fast_mps_choice.has_value()
                && fast_mps_choice->kind
                    == DeferredMinimizerIndexKind::per_sequence) {
                const auto rowno =
                    static_cast<std::uint64_t>(
                        scanner_pg_ctx->target_rownos[i]);
                // DOC0345 Patch J — real transient mps for the
                // no_index x per_sequence cell. This fast path
                // previously served per_sequence via the mpf-shaped
                // segmented aligner (minimap_port_segmented_*), i.e.
                // an mpf relabel — prohibited by DOC0345 §2 and the
                // §1.4 acceptance criterion. Build a genuine mps index
                // instead; the GPU sorted-pair seam (GPU emit + host
                // sort — DOC0348) streams from lazy PG (no whole-target
                // resident materialization, so 2/4 Gbp targets do not OOM).
                collection = build_and_align_transient_mps(
                    fq_pg,
                    ft_pg,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->total_bp_size));
                bioseq_log<DBG>("[bioseq] index: recompute real-mps-fast-noindex row={} index={} target_bp={} persist_compatible={} persisted=false backend={}",
                        static_cast<unsigned long long>(
                            rowno),
                        fast_mps_choice->index_name,
                        static_cast<unsigned long long>(
                            loc_opt->total_bp_size),
                        can_defer_fast_mps_recompute,
                        bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name(
                            target_backend_from_params_for_recompute(params)));
                assembled = true;
            }
        }
        if (!assembled) {
            const bool try_resident_segment_workers =
                target_bp_size > kPgSegmentationThresholdBp
                && !dispatch_params.enable_profiling
                && bioseq::parallelism::task_parallel(pool.mode())
                && pool.max_concurrency() > 1;
            if (try_resident_segment_workers) {
                auto resident =
                    try_materialize_target_for_segment_workers(
                        ft_pg,
                        static_cast<std::uint64_t>(
                            scanner_pg_ctx->target_rownos[i]));
                if (resident.has_value()) {
                    auto resident_target =
                        bioseq::storage::make_lazy_in_memory(
                            resident->range);
                    const bool can_defer_recompute_persist =
                        can_defer_recompute_minimizer_persist(
                            *scanner_pg_ctx, params);
                    const auto deferred_persist_choice =
                        can_defer_recompute_persist
                            ? choose_deferred_minimizer_persist_index(
                                *scanner_pg_ctx)
                            : std::optional<DeferredMinimizerPersistChoice>{};
                    if (deferred_persist_choice.has_value()) {
                        const auto rowno =
                            static_cast<std::uint64_t>(
                                scanner_pg_ctx->target_rownos[i]);
                        collection =
                            run_recompute_with_deferred_minimizer_persist(
                                lazy_query,
                                resident_target,
                                params,
                                *scanner_pg_ctx,
                                *loc_opt,
                                rowno,
                                alphabet_code,
                                storage_unit_byte_width,
                                static_cast<std::uint16_t>(
                                    _padding),
                                *deferred_persist_choice);
                    } else {
                        collection = minimap_port_segmented_pg_checked(
                            lazy_query, resident_target, params);
                        if (scanner_pg_ctx->recompute_async_persist_active
                            && (params.kmer_size != 15
                                || params.window_size != 10)) {
                            bioseq_log<DBG>("[bioseq] index: recompute async-persist skipped row={} target_bp={} because active params are k={} w={} but persisted minimizer indexes are pinned to k=15 w=10",
                                    static_cast<unsigned long long>(
                                        scanner_pg_ctx->target_rownos[i]),
                                    static_cast<unsigned long long>(
                                        loc_opt->total_bp_size),
                                    params.kmer_size,
                                    params.window_size);
                        } else if (can_defer_recompute_persist) {
                            bioseq_log<DBG>("[bioseq] index: recompute async-persist skipped row={} target_bp={} because no minimizer index is enabled for UPDATE_ON_INSERT",
                                    static_cast<unsigned long long>(
                                        scanner_pg_ctx->target_rownos[i]),
                                    static_cast<unsigned long long>(
                                        loc_opt->total_bp_size));
                        }
                    }
                } else {
                    collection = minimap_port_segmented_pg_checked(
                        fq_pg, ft_pg, params);
                    stage_recompute_mps_build_if_selected(
                        *scanner_pg_ctx,
                        *loc_opt,
                        static_cast<std::uint64_t>(
                            scanner_pg_ctx->target_rownos[i]),
                        alphabet_code,
                        storage_unit_byte_width,
                        static_cast<std::uint16_t>(_padding),
                        params,
                        "resident_materialize_failed");
                }
            } else {
                collection =
                    (target_bp_size > kPgSegmentationThresholdBp)
                        ? minimap_port_segmented_pg_checked(fq_pg, ft_pg, params)
                        : minimap_port_pg_checked(fq_pg, ft_pg, params);
                stage_recompute_mps_build_if_selected(
                    *scanner_pg_ctx,
                    *loc_opt,
                    static_cast<std::uint64_t>(
                        scanner_pg_ctx->target_rownos[i]),
                    alphabet_code,
                    storage_unit_byte_width,
                    static_cast<std::uint16_t>(_padding),
                    params,
                    "streaming_recompute");
            }
        }
    } else {
        compact_sequence_range<S> target_seq{*target_hdr};
        bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
            bioseq::storage::make_scanner_pg_in_memory<S>(target_seq)};
        collection =
            (target_bp_size > kPgSegmentationThresholdBp)
                ? minimap_port_segmented_pg_checked(fq_pg, ft_pg, params)
                : minimap_port_pg_checked(fq_pg, ft_pg, params);
    }

    if (row_consumed_persisted_query_sketch) {
        const auto& completed_query_proof = per_row_query_mode
            ? row_query_minimizer_proof
            : cached_query_minimizer_proof;
        if (!completed_query_proof.has_value()) {
            bioseq_report<ERR, PG>()
                .code(ERRCODE_DATA_CORRUPTED)
                .msg("consumed persisted query minimizers have no operation proof");
            __builtin_unreachable();
        }
        const bool exact_query_proof_absent =
            !collection.exact_simd_query_execution_proof_present
            && collection.exact_simd_query_execution_proof
                == decltype(collection.exact_simd_query_execution_proof){};
        const bool paper_query_proof_absent =
            !collection.paper_nthash_query_execution_proof_present
            && collection.paper_nthash_query_execution_proof
                == decltype(collection.paper_nthash_query_execution_proof){};
        if (!exact_query_proof_absent || !paper_query_proof_absent) {
            bioseq_report<ERR, PG>()
                .code(ERRCODE_INTERNAL_ERROR)
                .msg("persisted query sketch proof was recorded more than once");
            __builtin_unreachable();
        }
        if (completed_query_proof->exact_simd_present) {
            collection.record_exact_simd_query_execution_proof(
                completed_query_proof->exact_simd);
        }
        if (completed_query_proof->paper_nthash_present) {
            collection.record_paper_nthash_query_execution_proof(
                completed_query_proof->paper_nthash);
        }
        consumed_shared_cached_query_proof = !per_row_query_mode;
    }
    return collection;
}
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_ONLY)
template <class S>
[[nodiscard]] MatchResultCollection process_inmemory_standard_row_typed(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    compact_sequence_range<S> query_seq{*row_query_hdr};
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
    compact_sequence_range<S> target_seq{*target_hdr};
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
    return minimap_port_pg_checked(lazy_query, lazy_target, params);
}
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_ONLY)
template <class S>
[[nodiscard]] MatchResultCollection process_inmemory_segmented_row_typed(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    compact_sequence_range<S> query_seq{*row_query_hdr};
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
    compact_sequence_range<S> target_seq{*target_hdr};
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
    return minimap_port_segmented_pg_checked(lazy_query, lazy_target, params);
}
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_ONLY)
template <class S>
[[nodiscard]] MatchResultCollection process_inmemory_chunked_row_typed(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    compact_sequence_range<S> query_seq{*row_query_hdr};
    compact_sequence_range<S> target_seq{*target_hdr};
    using chunked = bioseq::storage::sequence_scanner_test_chunked<
        typename compact_sequence_range<S>::traits::storage_unit,
        bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    bioseq::storage::lazy_compact_sequence<S, chunked> fq{
        chunked::from_range(query_seq)};
    bioseq::storage::lazy_compact_sequence<S, chunked> ft{
        chunked::from_range(target_seq)};
    return minimap_port_pg_checked(fq, ft, params);
}
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_ONLY)
template <class S>
[[nodiscard]] MatchResultCollection process_inmemory_chunked_segmented_row_typed(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    compact_sequence_range<S> query_seq{*row_query_hdr};
    compact_sequence_range<S> target_seq{*target_hdr};
    using chunked = bioseq::storage::sequence_scanner_test_chunked<
        typename compact_sequence_range<S>::traits::storage_unit,
        bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    bioseq::storage::lazy_compact_sequence<S, chunked> fq{
        chunked::from_range(query_seq)};
    bioseq::storage::lazy_compact_sequence<S, chunked> ft{
        chunked::from_range(target_seq)};
    return minimap_port_segmented_pg_checked(fq, ft, params);
}
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_ONLY) \
    || (!defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_HELPER) \
        && !defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_HELPER_ONLY))
template <class S>
[[nodiscard]] MatchResultCollection process_inmemory_row_typed(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const sequence_size_t target_bp_size,
    const Minimap2Params& params,
    const bool chunked_scanner_forced)
{
#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_LEAF_HELPERS)
    const bool segmented = target_bp_size > kPgSegmentationThresholdBp;
    if (chunked_scanner_forced) {
        return segmented
            ? BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION(
                row_query_hdr, target_hdr, params)
            : BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION(
                row_query_hdr, target_hdr, params);
    }
    return segmented
        ? BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION(
            row_query_hdr, target_hdr, params)
        : BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION(
            row_query_hdr, target_hdr, params);
#else
    compact_sequence_range<S> query_seq{*row_query_hdr};
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
    compact_sequence_range<S> target_seq{*target_hdr};
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
    if (chunked_scanner_forced) {
        using chunked = bioseq::storage::sequence_scanner_test_chunked<
            typename compact_sequence_range<S>::traits::storage_unit,
            bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
        bioseq::storage::lazy_compact_sequence<S, chunked> fq{
            chunked::from_range(query_seq)};
        bioseq::storage::lazy_compact_sequence<S, chunked> ft{
            chunked::from_range(target_seq)};
        return (target_bp_size > kPgSegmentationThresholdBp)
            ? minimap_port_segmented_pg_checked(fq, ft, params)
            : minimap_port_pg_checked(fq, ft, params);
    }
    return (target_bp_size > kPgSegmentationThresholdBp)
        ? minimap_port_segmented_pg_checked(lazy_query, lazy_target, params)
        : minimap_port_pg_checked(lazy_query, lazy_target, params);
#endif
}
#endif

#if !defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_HELPER_ONLY)
template <class S>
[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_typed(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    const BatchInvocationRole invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<MatchResult>> results;
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;
    const bool record_target_operational_outcomes =
        invocation_role == BatchInvocationRole::primary
        && (scanner_pg_ctx == nullptr
            || scanner_pg_ctx->record_target_operational_outcomes);
	bool shared_target_outcome_recorded = false;

    const Minimap2Params& dispatch_params = base_params;

            const char* _force_chunked_guc =
                GetConfigOptionByName("bioseq.test_force_chunked_scanner", NULL, true);
            const bool chunked_scanner_forced =
                _force_chunked_guc && std::strcmp(_force_chunked_guc, "on") == 0;
            // DOC0264 §9.2 / DOC0274 — test parity knob. When on, the dispatch
            // loop forces the Phase B baseline (with_lazy_in_memory). Otherwise
            // (default) and given a non-null ScannerPgContext, the loop uses
            // the Phase C `sequence_scanner_pg<S>` (PG-backed for stub targets,
            // in-memory for the query literal / inline targets).
            const char* _force_inmemory_guc =
                GetConfigOptionByName("bioseq.test_force_inmemory_scanner", NULL, true);
            const bool inmemory_scanner_forced =
                _force_inmemory_guc && std::strcmp(_force_inmemory_guc, "on") == 0;
            const bool use_scanner_pg =
                scanner_pg_ctx != nullptr
                && !inmemory_scanner_forced
                && !chunked_scanner_forced;
            const auto index_read_requirement =
                scanner_pg_ctx != nullptr
                    ? scanner_pg_ctx->index_read_requirement
                    : bioseq::indexing::IndexReadRequirement::allow;
            const bool persisted_required =
                index_read_requirement
                == bioseq::indexing::IndexReadRequirement::
                    require_compatible_persisted;
			if (scanner_pg_ctx != nullptr) {
				using PolicySource = bioseq::storage::indexing::
					ActiveReadPolicyResolutionSource;
				const PolicySource source =
					scanner_pg_ctx->active_index_policy_source;
				const bool relation_backed =
					OidIsValid(scanner_pg_ctx->user_relid);
				const bool valid_relation_source = relation_backed
					&& (source == PolicySource::configured_row
						|| source
							== PolicySource::authoritative_no_row_default);
				const bool valid_non_relation_source = !relation_backed
					&& source == PolicySource::non_relation;
				if ((!valid_relation_source && !valid_non_relation_source)
					|| scanner_pg_ctx->active_index.empty()) {
					bioseq_report<ERR, PG>()
						.code(ERRCODE_INTERNAL_ERROR)
						.msg("active-read policy dispatch proof is invalid")
						.detail(
							"relation={} relation_backed={} source={} index={}",
							static_cast<unsigned>(scanner_pg_ctx->user_relid),
							relation_backed,
							bioseq::storage::indexing::
								active_read_policy_resolution_source_name(source),
							scanner_pg_ctx->active_index);
					__builtin_unreachable();
				}
			}
            if (index_read_requirement
                    == bioseq::indexing::IndexReadRequirement::require_absent
                && scanner_pg_ctx != nullptr
                && scanner_pg_ctx->read_index_active) {
                bioseq_report<ERR, PG>()
                    .code(ERRCODE_INTERNAL_ERROR)
                    .msg("index_read_requirement=require_absent contract violation")
                    .detail("typed processor received read_index_active=true");
                __builtin_unreachable();
            }
            if (persisted_required) {
                raise_required_persisted_index_error(
                    scanner_pg_ctx,
                    -1,
                    "no current MPF/MPS reader validates a revision-3 READY "
                    "identity; legacy payloads and warm caches are forbidden");
            }
			bioseq_log<DBG>("[bioseq] pg: BatchProcessor dispatching ({},{}) query_bp_size={} batch_size={} per_row_query={} use_scanner_pg={} chunked={} inmemory_forced={} active_index={} active_index_policy_source={}",
                    static_cast<unsigned>(alphabet_code),
                    static_cast<unsigned>(storage_unit_byte_width),
                    query_hdr != nullptr ? query_hdr->bp_size : 0,
                    batch_size, per_row_query_mode,
					use_scanner_pg, chunked_scanner_forced, inmemory_scanner_forced,
					scanner_pg_ctx != nullptr
						? std::string_view{scanner_pg_ctx->active_index}
						: std::string_view{"<no-scanner-context>"},
					scanner_pg_ctx != nullptr
						? bioseq::storage::indexing::
							active_read_policy_resolution_source_name(
								scanner_pg_ctx->active_index_policy_source)
						: std::string_view{"no_scanner_context"});

            /*
             * Query minimizers are computed once per batch for the
             * prebuilt-index path (every row reuses the same query).
             * Recompute-path rows use minimap_port_pg_checked(), which does its
             * own sketching; the extra allocation here is avoided
             * for those rows via the optional nested-scope.
             */
            std::optional<std::vector<MinimapSketchData>> cached_query_minimizers;
            std::optional<pipe::MinimizerRangeExecutionProof>
                cached_query_minimizer_proof;
            bool cached_query_minimizer_proof_operation_accounted = false;
            constexpr std::size_t kGpuBatchMinRows = 2;

            if (selected_vulkan_execution(dispatch_params)
                && batch_size < kGpuBatchMinRows) {
                add_operational_counter(
                    stats, stats.operational.backend_declines);
                bioseq::gpu::vulkan::VulkanRuntimeError err{
                    "selected Vulkan execution received a one-job wave; "
                    "silent CPU/SIMD fallthrough is forbidden"};
                raise_vulkan_pg_error("Vulkan batch cardinality", err.what());
            }

            if (selected_vulkan_execution(dispatch_params)
                       && batch_size >= kGpuBatchMinRows
                       && !chunked_scanner_forced) {
                const bool selected_vulkan_bounded_cigar =
                    dispatch_params.do_cigar
                    && dispatch_params.vulkan_extend_enabled
                    && selected_vulkan_coordinate_prereqs_active(dispatch_params);
                if ((!pipe::phase_c_coordinate_only_active(dispatch_params)
                     && !selected_vulkan_bounded_cigar)
                    || !pipe::phase_c_index_build_active(dispatch_params)) {
                    add_operational_counter(
                        stats, stats.operational.backend_declines);
                    bioseq::gpu::vulkan::VulkanRuntimeError err{
                        "selected Vulkan batch requires coordinate-only projection "
                        "or bounded GPU CIGAR extension, "
                        "Vulkan minimizers, Vulkan tiled-DP chaining, fused frontend, "
                        "and GPU target-index build"};
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }

                auto& runtime =
                    bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
                if (!runtime.usable()) {
                    add_operational_counter(
                        stats, stats.operational.backend_declines);
                    bioseq::gpu::vulkan::VulkanRuntimeError err{
                        "selected Vulkan batch runtime unavailable: "
                        + runtime.initialization_error()};
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }

                using MinimizerT = MinimapSketchData;
                struct GpuBatchQuerySidecar {
                    std::size_t row_index = 0;
                    sequence_size_t query_len = 0;
                };
                struct GpuBatchTargetSidecar {
                    const pgseq_t* hdr = nullptr;
                    std::uint64_t fingerprint = 0;
                    sequence_size_t target_len = 0;
                    sequence_size_t max_query_len = 0;
                    std::size_t row_count = 0;
                    std::vector<::bioseq::indexing::SegmentUnitRange> ranges{};
                    std::vector<std::shared_ptr<bioseq::indexing::GpuResidentMinimizerIndex>>
                        residents{};
                };
                struct GpuBatchProblemSidecar {
                    std::size_t row_index = 0;
                    std::size_t target_index = 0;
                    std::size_t segment_index = 0;
                    sequence_size_t core_lo_bp = 0;
                    sequence_size_t core_hi_bp = 0;
                };

                try {
                    const std::size_t w =
                        static_cast<std::size_t>(dispatch_params.window_size);
                    const std::size_t k =
                        static_cast<std::size_t>(dispatch_params.kmer_size);
                    // Declared before the detached host vectors so reverse
                    // destruction keeps their reconciled batch admission live
                    // until after both vectors release their storage.  The
                    // device span retains the same anchor during dispatch.
                    std::shared_ptr<bioseq::gpu::vulkan::
                        VulkanBatchSketchMemoryRetention>
                        query_sketch_memory_retention;
                    std::vector<MinimizerT> all_query_minimizers;
                    std::vector<bioseq::gpu::vulkan::PhaseCSeedQueryRecordWire>
                        query_records;
                    bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>
                        query_minimizer_device_span{};
                    std::vector<GpuBatchQuerySidecar> query_sidecars;
                    std::vector<std::uint32_t> row_to_query_index(
                        batch_size,
                        std::numeric_limits<std::uint32_t>::max());
                    pipe::PhaseCQueryGateStorage query_gate;
                    std::vector<bioseq::gpu::vulkan::SequenceUploadView>
                        batch_query_uploads;
                    const auto gpu_batch_now_ns = []() -> std::uint64_t {
                        return static_cast<std::uint64_t>(
                            bioseq::instr::clock::now_ns());
                    };
                    const std::uint64_t gpu_query_sketch_begin_ns =
                        gpu_batch_now_ns();

                    query_sidecars.reserve(batch_size);
                    batch_query_uploads.reserve(batch_size);
                    for (std::size_t i = 0; i < batch_size; ++i) {
                        const pgseq_t* row_query_hdr =
                            per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                        if (row_query_hdr == nullptr) {
                            continue;
                        }
                        if (row_query_hdr->alphabet_code != alphabet_code
                            || row_query_hdr->storage_unit_byte_width
                                != storage_unit_byte_width) {
                            continue;
                        }
                        compact_sequence_range<S> query_seq{*row_query_hdr};
                        const std::uint32_t query_index =
                            static_cast<std::uint32_t>(batch_query_uploads.size());
                        batch_query_uploads.push_back(
                            bioseq::gpu::vulkan::make_sequence_upload_view(query_seq));
                        query_sidecars.push_back(GpuBatchQuerySidecar{
                            .row_index = i,
                            .query_len = query_seq.bp_size(),
                        });
                        row_to_query_index[i] = query_index;
                    }
                    if (!batch_query_uploads.empty()) {
                        const auto rank_lut =
                            bioseq::gpu::vulkan::make_vulkan_rank_lut<
                                compact_sequence_range<S>>();
                        auto gpu_batch_sketch =
                            bioseq::gpu::vulkan::
                                collect_minimap_sketch_data_vulkan_upload_batch_result(
                                    std::span<
                                        const bioseq::gpu::vulkan::SequenceUploadView>{
                                        batch_query_uploads.data(),
                                        batch_query_uploads.size()},
                                    rank_lut,
                                    w,
                                    k,
                                    bioseq::gpu::vulkan::
                                        VulkanSketchHostMaterialization::
                                            host_and_device);
                        if (!gpu_batch_sketch) {
                            pipe::throw_vulkan_declined(
                                "Vulkan batched query minimizer sketch");
                        }
                        all_query_minimizers =
                            std::move(gpu_batch_sketch->host_minimizers);
                        query_records = std::move(gpu_batch_sketch->query_records);
                        query_sketch_memory_retention =
                            gpu_batch_sketch->memory_retention;
                        query_minimizer_device_span =
                            gpu_batch_sketch->device_span();
                        for (const auto& qrec : query_records) {
                            if (qrec.minimizer_count == 0u) {
                                continue;
                            }
                            const std::size_t begin = qrec.minimizer_offset;
                            const std::size_t end =
                                begin + static_cast<std::size_t>(qrec.minimizer_count);
                            if (end > all_query_minimizers.size()) {
                                throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                    "batched query minimizer record exceeds host buffer"};
                            }
                            query_gate.add(std::span<const MinimizerT>{
                                all_query_minimizers.data() + begin,
                                static_cast<std::size_t>(qrec.minimizer_count)});
                        }
                    }
                    add_stage_host_time(
                        stats,
                        ::bioseq::pipeline_stats::PipelineStage::query_sketch,
                        gpu_batch_now_ns() - gpu_query_sketch_begin_ns);
                    add_operational_counter(
                        stats,
                        stats.operational.host_query_hash_readbacks);

                    std::vector<GpuBatchTargetSidecar> targets;
                    std::vector<std::uint32_t> row_to_target_index(
                        batch_size,
                        std::numeric_limits<std::uint32_t>::max());
                    targets.reserve(batch_size);
                    const auto target_storage_bytes =
                        [] (const pgseq_t* hdr) -> std::string_view {
                        compact_sequence_range<S> seq{*hdr};
                        using StorageUnit =
                            typename compact_sequence_range<S>::traits::storage_unit;
                        const auto* bytes =
                            reinterpret_cast<const char*>(seq.cbegin());
                        return std::string_view{
                            bytes,
                            seq.size() * sizeof(StorageUnit)};
                    };
                    const auto target_fingerprint =
                        [target_storage_bytes] (
                            const pgseq_t* hdr) -> std::uint64_t {
                        std::uint64_t hash = 14695981039346656037ull;
                        hash = bioseq::common::hash_combine_64(
                            hash, static_cast<std::uint64_t>(hdr->alphabet_code));
                        hash = bioseq::common::hash_combine_64(
                            hash, static_cast<std::uint64_t>(
                                hdr->storage_unit_byte_width));
                        hash = bioseq::common::hash_combine_64(
                            hash, static_cast<std::uint64_t>(hdr->bp_size));
                        return bioseq::common::hash_combine_64(
                            hash, target_storage_bytes(hdr));
                    };
	                    for (std::size_t i = 0; i < batch_size; ++i) {
	                        if (row_to_query_index[i]
	                            == std::numeric_limits<std::uint32_t>::max()) {
	                            continue;
	                        }
	                        const pgseq_t* target_hdr = target_hdrs[i];
	                        if (target_hdr == nullptr) {
	                            if (scanner_pg_ctx != nullptr
	                                && scanner_pg_ctx->target_locators[i].has_value()) {
	                                const auto& loc =
	                                    *scanner_pg_ctx->target_locators[i];
	                                throw bioseq::gpu::vulkan::VulkanRuntimeError(
	                                    fmt::format(
	                                        "selected Vulkan batch target row {} "
	                                        "is only available as a PG locator "
	                                        "({} bp); selected Vulkan requires a "
	                                        "materialized target payload and "
	                                        "cannot use scanner_pg/MaxAllocSize "
	                                        "streaming",
	                                        static_cast<int>(query_ids[i]),
	                                        static_cast<unsigned long long>(
	                                            loc.total_bp_size)));
	                            }
	                            continue;
	                        }
	                        if (target_hdr->alphabet_code != alphabet_code
	                            || target_hdr->storage_unit_byte_width
	                                != storage_unit_byte_width) {
	                            continue;
	                        }
	                        std::uint32_t target_index =
	                            std::numeric_limits<std::uint32_t>::max();
	                        for (std::size_t existing = 0;
	                             existing < targets.size();
	                             ++existing) {
	                            if (targets[existing].hdr == target_hdr) {
	                                target_index = static_cast<std::uint32_t>(existing);
	                                break;
	                            }
	                        }
	                        std::uint64_t fingerprint = 0u;
	                        if (target_index
	                            == std::numeric_limits<std::uint32_t>::max()) {
	                            fingerprint = target_fingerprint(target_hdr);
	                        }
	                        for (std::size_t existing = 0;
	                             target_index
	                                 == std::numeric_limits<std::uint32_t>::max()
	                             && existing < targets.size();
	                             ++existing) {
	                            if (targets[existing].fingerprint == fingerprint
	                                && targets[existing].target_len
	                                    == static_cast<sequence_size_t>(
	                                        target_hdr->bp_size)
                                && targets[existing].hdr != nullptr
                                && target_storage_bytes(targets[existing].hdr)
                                    == target_storage_bytes(target_hdr)) {
                                target_index = static_cast<std::uint32_t>(existing);
                                break;
                            }
                        }
                        const sequence_size_t row_query_len =
                            query_sidecars[row_to_query_index[i]].query_len;
                        if (target_index
                            == std::numeric_limits<std::uint32_t>::max()) {
                            target_index =
                                static_cast<std::uint32_t>(targets.size());
                            targets.push_back(GpuBatchTargetSidecar{
                                .hdr = target_hdr,
                                .fingerprint = fingerprint,
                                .target_len =
                                    static_cast<sequence_size_t>(target_hdr->bp_size),
                                .max_query_len = row_query_len,
                                .row_count = 1,
                                .ranges = {},
                                .residents = {},
                            });
                        } else {
                            ++targets[target_index].row_count;
                            targets[target_index].max_query_len =
                                std::max(
                                    targets[target_index].max_query_len,
                                    row_query_len);
                        }
                        row_to_target_index[i] = target_index;
                    }
                    const auto requested_source =
                        requested_target_index_source(scanner_pg_ctx);
                    const auto record_gpu_batch_outcomes =
                        [batch_size,
                         broadcast_query_ordinal,
                         per_row_query_mode,
                         record_target_operational_outcomes,
                         requested_source,
                         scanner_pg_ctx,
                         &dispatch_params,
                         &row_to_query_index,
                         &row_to_target_index,
                         &cached_query_minimizer_proof_operation_accounted,
                         &stats] {
                        std::vector<std::uint64_t> logical_target_rows;
                        if (record_target_operational_outcomes) {
                            logical_target_rows.reserve(batch_size);
                        }
                        for (std::size_t row = 0; row < batch_size; ++row) {
                            if (row_to_query_index[row]
                                    == std::numeric_limits<std::uint32_t>::max()
                                || row_to_target_index[row]
                                    == std::numeric_limits<std::uint32_t>::max()) {
                                continue;
                            }
                            if (record_target_operational_outcomes) {
                                const std::uint64_t logical_target_row =
                                    scanner_pg_ctx != nullptr
                                        && scanner_pg_ctx->target_rownos[row] >= 0
                                    ? static_cast<std::uint64_t>(
                                        scanner_pg_ctx->target_rownos[row])
                                    : static_cast<std::uint64_t>(row);
                                logical_target_rows.push_back(logical_target_row);
                            }
                            record_unattested_job(
                                stats,
                                dispatch_params,
                                requested_source,
                                ::bioseq::pipeline_stats::TargetIndexSource::recomputed,
                                scanner_pg_ctx,
                                row,
                                per_row_query_mode,
                                broadcast_query_ordinal,
                                /*execution_proof=*/nullptr,
                                /*consumed_shared_cached_query_proof=*/false,
                                cached_query_minimizer_proof_operation_accounted);
                        }
                        if (record_target_operational_outcomes) {
                            const auto unique_target_count =
                                ::bioseq::pipeline_stats::
                                    count_unique_logical_target_rows(
                                        std::span<std::uint64_t>{
                                            logical_target_rows});
                            record_target_row(stats, unique_target_count);
                            record_target_index_outcome(
                                stats,
                                requested_source,
                                ::bioseq::pipeline_stats::TargetIndexSource::recomputed,
                                unique_target_count);
                        }
                    };
                    if (query_records.empty() || targets.empty()) {
                        record_gpu_batch_outcomes();
                        return results;
                    }

                    const auto frozen_query_gate =
                        pipe::freeze_phase_c_query_gate(
                            query_gate.vulkan_gate());
                    const auto frozen_gate_identity =
                        frozen_query_gate.canonical_identity();

                    constexpr std::size_t lpu =
                        compact_sequence_range<S>::traits::letters_per_unit;
                    for (std::size_t target_index = 0;
                         target_index < targets.size();
                         ++target_index) {
                        GpuBatchTargetSidecar& target = targets[target_index];
                        compact_sequence_range<S> target_seq{*target.hdr};
                        const std::uint64_t gpu_target_plan_begin_ns =
                            gpu_batch_now_ns();
                        constexpr sequence_size_t kGpuBatchHugeTargetBp =
                            100'000'000ull;
                        constexpr sequence_size_t kGpuBatchHugeTargetCoreBp =
                            8'000'000ull;
                        constexpr sequence_size_t kGpuBatchHugeTargetOverlapMaxBp =
                            30'000'000ull;
                        if (target.row_count > 1
                            && target.target_len >= kGpuBatchHugeTargetBp) {
                            target.ranges.clear();
                            const sequence_size_t total_units =
                                (target.target_len + lpu - 1u) / lpu;
                            sequence_size_t overlap_bp =
                                target.max_query_len
                                + target.max_query_len / 4u
                                + 100'000ull;
                            overlap_bp = std::min(
                                overlap_bp,
                                kGpuBatchHugeTargetOverlapMaxBp);
                            const sequence_size_t overlap_units =
                                (overlap_bp + lpu - 1u) / lpu;
                            const sequence_size_t segment_units =
                                std::max<sequence_size_t>(
                                    1u,
                                    kGpuBatchHugeTargetCoreBp / lpu);
                            for (sequence_size_t lo_u = 0; lo_u < total_units;
                                 lo_u += segment_units) {
                                const sequence_size_t hi_u =
                                    std::min(lo_u + segment_units, total_units);
                                const sequence_size_t cs_u =
                                    lo_u > overlap_units ? lo_u - overlap_units : 0u;
                                const sequence_size_t ce_u =
                                    std::min(hi_u + overlap_units, total_units);
                                target.ranges.push_back(::bioseq::indexing::SegmentUnitRange{
                                    .cs_u = cs_u,
                                    .ce_u = ce_u,
                                    .core_lo_bp = lo_u * lpu,
                                    .core_hi_bp = std::min(hi_u * lpu,
                                                           target.target_len),
                                });
                                if (hi_u == total_units) {
                                    break;
                                }
                            }
                        } else {
                            target.ranges = pipe::segment_unit_ranges_for_backend(
                                target.target_len,
                                target.max_query_len,
                                lpu,
                                MinimizerSketchBackend::VulkanSegment);
                        }
                        add_stage_host_time(
                            stats,
                            ::bioseq::pipeline_stats::PipelineStage::target_index_build,
                            gpu_batch_now_ns() - gpu_target_plan_begin_ns);
                        target.residents.reserve(target.ranges.size());
                        for (std::size_t segment_index = 0;
                             segment_index < target.ranges.size();
                             ++segment_index) {
                            const ::bioseq::indexing::SegmentUnitRange& range =
                                target.ranges[segment_index];
                            const std::uint64_t gpu_index_build_begin_ns =
                                gpu_batch_now_ns();
                            const bool full_target_range =
                                range.cs_u == 0
                                && static_cast<std::size_t>(range.ce_u)
                                    >= target_seq.size();
                            auto built = full_target_range
                                ? bioseq::gpu::vulkan::build_gpu_index_on_device<
                                    MinimizerT>(
                                    target_seq, w, k, frozen_query_gate)
                                : bioseq::gpu::vulkan::build_gpu_segment_index_on_device<
                                    MinimizerT>(
                                    target_seq,
                                    static_cast<std::size_t>(range.cs_u),
                                    static_cast<std::size_t>(range.ce_u),
                                    w,
                                    k,
                                    frozen_query_gate);
                            if (!built) {
                                pipe::throw_vulkan_declined(
                                    "Vulkan batched segment index build");
                            }
                            if (!built->valid || built->position_count == 0u
                                || built->occupied_slots == 0u) {
                                add_stage_host_time(
                                    stats,
                                    ::bioseq::pipeline_stats::PipelineStage::target_index_build,
                                    gpu_batch_now_ns()
                                        - gpu_index_build_begin_ns);
                                target.residents.push_back(nullptr);
                                continue;
                            }
                            bioseq::indexing::GpuMinimizerIndexCacheKey key{
                                .source_id = static_cast<std::uint64_t>(
                                    target_index + 1u),
                                .caller_identity =
                                    0xd1b54a32d192ed03ULL
                                    ^ (static_cast<std::uint64_t>(segment_index)
                                       + 0x9e3779b97f4a7c15ULL),
                                .target_len = built->target_len,
                                .table_size = built->occupied_slots,
                                .position_count = built->position_count,
                            };
                            pipe::bind_verified_gpu_segment_gate_identity(
                                key, frozen_gate_identity, built->gate_identity);
                            auto adopted =
                                bioseq::indexing::adopt_gpu_built_index(
                                    runtime, std::move(*built), key);
                            if (!adopted) {
                                throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                    fmt::format(
                                        "Vulkan batched segment index adoption "
                                        "rejected ({})",
                                        bioseq::indexing::
                                            gpu_built_index_adoption_error_name(
                                                adopted.error())));
                            }
                            auto resident = std::move(*adopted);
                            resident->target_position_offset = range.cs_u * lpu;
                            target.residents.push_back(resident);
                            add_stage_host_time(
                                stats,
                                ::bioseq::pipeline_stats::PipelineStage::target_index_build,
                                gpu_batch_now_ns() - gpu_index_build_begin_ns);
                        }
                    }

                    std::vector<bioseq::gpu::vulkan::PhaseCSeedCoordinateJob>
                        gpu_jobs;
                    std::vector<GpuBatchProblemSidecar> problem_sidecars;
                    const std::uint64_t gpu_problem_build_begin_ns =
                        gpu_batch_now_ns();
	                    gpu_jobs.reserve(batch_size);
	                    problem_sidecars.reserve(batch_size);
	                    for (std::size_t target_index = 0;
	                         target_index < targets.size();
	                         ++target_index) {
	                        GpuBatchTargetSidecar& target = targets[target_index];
	                        for (std::size_t segment_index = 0;
	                             segment_index < target.residents.size();
	                             ++segment_index) {
	                            const auto& resident = target.residents[segment_index];
	                            if (!resident) {
	                                continue;
	                            }
	                            const ::bioseq::indexing::SegmentUnitRange& range =
	                                target.ranges[segment_index];
	                            for (std::size_t row = 0; row < batch_size; ++row) {
	                                const std::uint32_t query_index =
	                                    row_to_query_index[row];
	                                if (query_index
	                                        == std::numeric_limits<std::uint32_t>::max()
	                                    || row_to_target_index[row] != target_index) {
	                                    continue;
	                                }
	                                gpu_jobs.push_back(
	                                    bioseq::gpu::vulkan::
	                                        PhaseCSeedCoordinateJob{
	                                        .slots = &resident->slots,
	                                        .positions = &resident->positions,
	                                        .target_len = resident->target_len,
	                                        .slot_mask = resident->slot_mask,
	                                        .position_count =
	                                            resident->position_count,
	                                        .bucket_count =
	                                            resident->bucket_count,
	                                        .target_position_offset =
	                                            resident->target_position_offset,
	                                        .query_index = query_index,
	                                    });
	                                problem_sidecars.push_back(GpuBatchProblemSidecar{
	                                    .row_index = row,
	                                    .target_index = target_index,
	                                    .segment_index = segment_index,
	                                    .core_lo_bp = range.core_lo_bp,
	                                    .core_hi_bp = range.core_hi_bp,
	                                });
	                            }
	                        }
	                    }
                    add_stage_host_time(
                        stats,
                        ::bioseq::pipeline_stats::PipelineStage::seed_lookup,
                        gpu_batch_now_ns() - gpu_problem_build_begin_ns);

                    if (gpu_jobs.empty()) {
                        record_gpu_batch_outcomes();
                        return results;
                    }

                    auto cp = dispatch_params.to_chaining_params();
                    std::int32_t max_dist_x = cp.max_gap;
                    std::int32_t max_dist_y = cp.max_gap;
                    if (max_dist_x < cp.bandwidth) {
                        max_dist_x = cp.bandwidth;
                    }
                    if (max_dist_y < cp.bandwidth) {
                        max_dist_y = cp.bandwidth;
                    }
                    const auto chain_key =
                        bioseq::chaining::choose_vulkan_chaining_pipeline(
                            runtime.features());
                    std::span<const MinimizerT> minimizer_span{
                        all_query_minimizers.data(),
                        all_query_minimizers.size()};
                    bioseq::gpu::vulkan::PhaseCSeedQueryInputs query_inputs{
                        .host_minimizer_bytes =
                            query_minimizer_device_span.words() == nullptr
                                ? std::as_bytes(minimizer_span)
                                : std::span<const std::byte>{},
                        .device_minimizer_words =
                            query_minimizer_device_span.words(),
                        .minimizer_count = static_cast<std::uint32_t>(
                            all_query_minimizers.size()),
                        .minimizer_word_kind =
                            static_cast<std::uint32_t>(
                                bioseq::gpu::vulkan::minimizer_wire_traits<
                                    MinimizerT>::word_kind),
                        .minimizer_words32_per_record =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                MinimizerT>::words32_per_minimizer,
                        .hash_shift =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                MinimizerT>::hash_shift,
                        .pos_bits =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                MinimizerT>::pos_bits,
                        .host_query_records = std::span<
                            const bioseq::gpu::vulkan::PhaseCSeedQueryRecordWire>{
                            query_records.data(),
                            query_records.size()},
                        .device_query_records = nullptr,
                    };
                    const auto coord_run =
                        runtime.run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
                            query_inputs,
                            std::span<const bioseq::gpu::vulkan::PhaseCSeedCoordinateJob>{
                                gpu_jobs.data(),
                                gpu_jobs.size()},
                            static_cast<std::uint32_t>(dispatch_params.kmer_size),
                            dispatch_params.max_occ,
                            bioseq::gpu::vulkan::PhaseCSeedCoordinateAlgorithm::
                                vulkan_tiled_dp,
                            chain_key,
                            max_dist_x,
                            max_dist_y,
                            cp.bandwidth,
                            std::min<std::int32_t>(cp.max_chain_iter, 3000),
                            cp.gap_penalty,
                            cp.skip_penalty,
                            cp.max_chain_skip,
                            cp.min_chain_score,
                            cp.min_seed_count,
                            16u);
                    add_operational_counter(
                        stats,
                        stats.operational.gpu_seed_dispatches,
                        coord_run.stats.seed_count_dispatches
                            + coord_run.stats.seed_emit_dispatches);
                    if (coord_run.stats.coordinate_vote_dispatches != 0u) {
                        throw bioseq::gpu::vulkan::VulkanRuntimeError(
                            "selected Vulkan tiled-DP route executed the "
                            "coordinate-vote algorithm");
                    }
                    add_operational_counter(
                        stats,
                        stats.operational.gpu_chain_dispatches,
                        coord_run.stats.chain_dispatches);
                    add_operational_counter(
                        stats,
                        stats.operational.gpu_chain_backtrack_dispatches,
                        coord_run.stats.chain_backtrack_dispatches);
                    add_operational_counter(
                        stats,
                        stats.operational.gpu_coordinate_vote_dispatches,
                        coord_run.stats.coordinate_vote_dispatches);

                    struct GpuCigarPendingMatch {
                        int32_t query_id = 0;
                        MatchResult match{};
                    };
                    std::vector<GpuCigarPendingMatch> pending_cigar_matches;
                    std::vector<bioseq::gpu::vulkan::GpuAlignmentCigarJobWire>
                        cigar_jobs;
                    std::vector<std::uint32_t> cigar_query_symbols;
                    std::vector<std::uint32_t> cigar_target_symbols;
                    std::vector<
                        typename compact_sequence_range<S>::traits::letter_unit>
                        cigar_symbol_scratch;
                    std::uint32_t packed_cigar_capacity = 0;
                    std::uint32_t traceback_word_capacity = 0;
                    pending_cigar_matches.reserve(coord_run.results.size());
                    cigar_jobs.reserve(coord_run.results.size());
                    const auto gpu_cigar_now_ns = []() -> std::uint64_t {
                        return static_cast<std::uint64_t>(
                            bioseq::instr::clock::now_ns());
                    };
                    const std::uint64_t cigar_prepare_begin_ns =
                        gpu_cigar_now_ns();

                    for (std::size_t problem_index = 0;
                         problem_index < coord_run.results.size();
                         ++problem_index) {
                        const auto& coord = coord_run.results[problem_index];
                        if (coord.valid == 0u) {
                            continue;
                        }
                        const GpuBatchProblemSidecar& owner =
                            problem_sidecars[problem_index];
                        if (coord.target_start < owner.core_lo_bp
                            || coord.target_start >= owner.core_hi_bp) {
                            continue;
                        }
                        const std::uint32_t query_index =
                            row_to_query_index[owner.row_index];
                        const GpuBatchTargetSidecar& target =
                            targets[owner.target_index];
                        MatchResult result =
                            pipe::phase_c_coordinate_to_match_result(
                                coord,
                                query_sidecars[query_index].query_len,
                                target.target_len);
                        if (!dispatch_params.do_cigar) {
                            results[query_ids[owner.row_index]].push_back(
                                std::move(result));
                            continue;
                        }

                        const pgseq_t* row_query_hdr =
                            per_row_query_mode
                                ? (*per_row_query_hdrs)[owner.row_index]
                                : query_hdr;
                        if (row_query_hdr == nullptr || target.hdr == nullptr) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                "GPU CIGAR extension received null query/target sequence");
                        }
                        compact_sequence_range<S> row_query_seq{*row_query_hdr};
                        compact_sequence_range<S> target_seq{*target.hdr};
                        const sequence_size_t query_len_bp =
                            row_query_seq.bp_size();
                        const sequence_size_t target_len_bp =
                            target_seq.bp_size();
                        if (result.query_start > query_len_bp
                            || result.target_start > target_len_bp) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                "GPU CIGAR extension coordinate start exceeds sequence length");
                        }
                        result.query_end =
                            std::min(result.query_end, query_len_bp);
                        result.target_end =
                            std::min(result.target_end, target_len_bp);
                        const sequence_size_t query_span =
                            result.query_end - result.query_start;
                        const sequence_size_t target_span =
                            result.target_end - result.target_start;
                        if (query_span == 0 || target_span == 0) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                "GPU CIGAR extension received empty coordinate span");
                        }
                        const auto checked_u32 =
                            [] (std::uint64_t value,
                                std::string_view what) -> std::uint32_t {
                            if (value > std::numeric_limits<std::uint32_t>::max()) {
                                throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                    fmt::format("{} exceeds uint32", what));
                            }
                            return static_cast<std::uint32_t>(value);
                        };
                        if (query_span
                                > bioseq::gpu::vulkan::
                                    k_gpu_alignment_simple_cigar_max_len
                            || target_span
                                > bioseq::gpu::vulkan::
                                    k_gpu_alignment_simple_cigar_max_len) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                fmt::format(
                                    "GPU CIGAR extension simple CIGAR supports spans up to {}, "
                                    "got query_span={} target_span={}",
                                    bioseq::gpu::vulkan::
                                        k_gpu_alignment_simple_cigar_max_len,
                                    query_span,
                                    target_span));
                        }

                        const std::uint32_t query_symbol_offset =
                            checked_u32(cigar_query_symbols.size(),
                                        "GPU CIGAR query symbol offset");
                        std::span<const std::uint32_t> query_symbols =
                            append_gpu_alignment_symbols(
                                cigar_query_symbols,
                                cigar_symbol_scratch,
                                row_query_seq,
                                result.query_start,
                                result.query_end,
                                false);
                        const std::uint32_t target_symbol_offset =
                            checked_u32(cigar_target_symbols.size(),
                                        "GPU CIGAR target symbol offset");
                        std::span<const std::uint32_t> target_symbols =
                            append_gpu_alignment_symbols(
                                cigar_target_symbols,
                                cigar_symbol_scratch,
                                target_seq,
                                result.target_start,
                                result.target_end,
                                result.is_reverse);
                        add_operational_counter(
                            stats,
                            stats.operational.host_query_symbol_packing_calls);
                        add_operational_counter(
                            stats,
                            stats.operational.host_target_symbol_packing_calls);

                        const std::uint32_t query_len =
                            checked_u32(query_span, "GPU CIGAR query span");
                        const std::uint32_t target_len =
                            checked_u32(target_span, "GPU CIGAR target span");
                        const std::uint32_t cigar_capacity =
                            std::max<std::uint32_t>(
                                1u,
                                checked_u32(
                                    query_span + target_span,
                                    "GPU CIGAR packed capacity"));
                        const bool bounded_dp_candidate =
                            query_len
                                <= bioseq::gpu::vulkan::
                                    k_gpu_alignment_extension_max_len
                            && target_len
                                <= bioseq::gpu::vulkan::
                                    k_gpu_alignment_extension_max_len;
                        const bool exact_long_cigar_candidate =
                            query_len == target_len
                            && std::equal(
                                query_symbols.begin(), query_symbols.end(),
                                target_symbols.begin());
                        const bool unit_gap_walk_cigar_candidate =
                            gpu_alignment_unit_gap_walk_cigar_candidate(
                                query_symbols, target_symbols);
                        const std::uint64_t traceback_cells =
                            bounded_dp_candidate
                                ? (static_cast<std::uint64_t>(query_len) + 1u)
                                    * (static_cast<std::uint64_t>(target_len) + 1u)
                            : (exact_long_cigar_candidate
                               || unit_gap_walk_cigar_candidate)
                                ? 0u
                                : bioseq::gpu::vulkan::
                                    gpu_alignment_cigar_gridded_traceback_words(
                                        query_len,
                                        target_len,
                                        bioseq::gpu::vulkan::
                                            k_gpu_alignment_cigar_traceback_tile_len,
                                        bioseq::gpu::vulkan::
                                            k_gpu_alignment_cigar_traceback_tile_len);
                        add_operational_counter(
                            stats,
                            stats.operational
                                .host_alignment_job_classification_calls);
                        cigar_jobs.push_back(
                            bioseq::gpu::vulkan::GpuAlignmentCigarJobWire{
                                .query_symbol_offset = query_symbol_offset,
                                .target_symbol_offset = target_symbol_offset,
                                .query_len = query_len,
                                .target_len = target_len,
                                .query_global_start = checked_u32(
                                    result.query_start,
                                    "GPU CIGAR query global start"),
                                .target_global_start = checked_u32(
                                    result.target_start,
                                    "GPU CIGAR target global start"),
                                .band_width = std::min<std::uint32_t>(
                                    bioseq::gpu::vulkan::
                                        k_gpu_alignment_extension_max_band_width,
                                    std::max(query_len, target_len)),
                                .flags = 0,
                                .cigar_offset = packed_cigar_capacity,
                                .cigar_capacity = cigar_capacity,
                                .traceback_tile_offset =
                                    traceback_word_capacity,
                                .traceback_tile_count = checked_u32(
                                    traceback_cells,
                                    "GPU CIGAR traceback cell count"),
                            });
                        packed_cigar_capacity = checked_u32(
                            static_cast<std::uint64_t>(packed_cigar_capacity)
                                + cigar_capacity,
                            "GPU CIGAR packed capacity total");
                        traceback_word_capacity = checked_u32(
                            static_cast<std::uint64_t>(traceback_word_capacity)
                                + traceback_cells,
                            "GPU CIGAR traceback capacity total");
                        pending_cigar_matches.push_back(GpuCigarPendingMatch{
                            .query_id = query_ids[owner.row_index],
                            .match = std::move(result),
                        });
                    }
                    add_stage_host_time(
                        stats,
                        ::bioseq::pipeline_stats::PipelineStage::cigar_job_generation,
                        gpu_cigar_now_ns() - cigar_prepare_begin_ns);

                    if (dispatch_params.do_cigar && !cigar_jobs.empty()) {
                        const bioseq::gpu::vulkan::GpuAlignmentCigarParams
                            cigar_params{
                                .alphabet_size = static_cast<std::uint32_t>(
                                    compact_sequence_range<S>::traits::
                                        alphabet_size_n),
                                .max_band_width = bioseq::gpu::vulkan::
                                    k_gpu_alignment_extension_max_band_width,
                                .tile_query = bioseq::gpu::vulkan::
                                    k_gpu_alignment_cigar_traceback_tile_len,
                                .tile_target = bioseq::gpu::vulkan::
                                    k_gpu_alignment_cigar_traceback_tile_len,
                                .match_score = dispatch_params.match_score,
                                .mismatch_penalty = dispatch_params.mismatch_pen,
                                .gap_open_short = dispatch_params.gap_open1,
                                .gap_extend_short = dispatch_params.gap_ext1,
                                .gap_open_long = dispatch_params.gap_open2,
                                .gap_extend_long = dispatch_params.gap_ext2,
                                .z_drop = dispatch_params.zdrop,
                                .end_bonus = std::max<std::int32_t>(
                                    0,
                                    dispatch_params.end_bonus),
                                .flags = 0,
                                .reserved = 0,
                            };
                        const auto cigar_run =
                            runtime.run_alignment_simple_cigar_roundtrip(
                                std::span<const std::uint32_t>{
                                    cigar_query_symbols.data(),
                                    cigar_query_symbols.size()},
                                std::span<const std::uint32_t>{
                                    cigar_target_symbols.data(),
                                    cigar_target_symbols.size()},
                                std::span<
                                    const bioseq::gpu::vulkan::
                                        GpuAlignmentCigarJobWire>{
                                    cigar_jobs.data(),
                                    cigar_jobs.size()},
                                cigar_params,
                                packed_cigar_capacity);
                        add_operational_counter(
                            stats, stats.operational.gpu_extend_dispatches);
                        if (cigar_run.results.size()
                            != pending_cigar_matches.size()) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                "GPU CIGAR extension result count mismatch");
                        }
                        for (std::size_t i = 0;
                             i < pending_cigar_matches.size();
                             ++i) {
                            const auto& cigar_result = cigar_run.results[i];
                            const auto& cigar_job = cigar_jobs[i];
                            if (cigar_result.cigar_length == 0u) {
                                throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                    "GPU CIGAR extension returned empty CIGAR");
                            }
                            const std::uint64_t cigar_end =
                                static_cast<std::uint64_t>(
                                    cigar_result.cigar_offset)
                                + static_cast<std::uint64_t>(
                                    cigar_result.cigar_length);
                            if (cigar_end > cigar_run.packed_cigar.size()) {
                                throw bioseq::gpu::vulkan::VulkanRuntimeError(
                                    "GPU CIGAR extension returned invalid CIGAR slice");
                            }

                            GpuCigarPendingMatch& pending =
                                pending_cigar_matches[i];
                            MatchResult& match = pending.match;
                            match.dp_score = cigar_result.score;
                            match.dp_max_score = cigar_result.max_score;
                            match.query_start = cigar_result.query_start;
                            match.query_end = cigar_result.query_end;
                            match.target_start = cigar_result.target_start;
                            match.target_end = cigar_result.target_end;
                            match.has_cigar = true;
                            match.cigar.clear();

                            const std::span<const std::uint32_t> packed{
                                cigar_run.packed_cigar.data()
                                    + cigar_result.cigar_offset,
                                cigar_result.cigar_length};
                            sequence_size_t block_length = 0;
                            sequence_size_t num_matches = 0;
                            std::uint32_t query_cursor = 0;
                            std::uint32_t target_cursor = 0;
                            for (std::uint32_t entry : packed) {
                                const std::uint32_t opcode = entry & 0xfU;
                                const std::uint32_t len = entry >> 4;
                                if (opcode == CIGAR_MATCH
                                    || opcode == CIGAR_EQ
                                    || opcode == CIGAR_X) {
                                    block_length += len;
                                    const std::uint32_t q_base =
                                        cigar_job.query_symbol_offset
                                        + query_cursor;
                                    const std::uint32_t t_base =
                                        cigar_job.target_symbol_offset
                                        + target_cursor;
                                    for (std::uint32_t j = 0; j < len; ++j) {
                                        if (cigar_query_symbols[q_base + j]
                                            == cigar_target_symbols[
                                                t_base + j]) {
                                            ++num_matches;
                                        }
                                    }
                                    query_cursor += len;
                                    target_cursor += len;
                                } else if (opcode == CIGAR_INS) {
                                    block_length += len;
                                    query_cursor += len;
                                } else if (opcode == CIGAR_DEL) {
                                    block_length += len;
                                    target_cursor += len;
                                }
                            }
                            match.block_length = block_length;
                            match.num_matches = num_matches;
                            append_packed_cigar_slice(match.cigar, packed);
                            results[pending.query_id].push_back(
                                std::move(match));
                        }
                        add_operational_counter(
                            stats,
                            stats.operational.host_cigar_construction_calls,
                            static_cast<std::uint64_t>(
                                pending_cigar_matches.size()));
                    }
                    record_gpu_batch_outcomes();
                    return results;
                } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& err) {
                    add_operational_counter(
                        stats, stats.operational.backend_declines);
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }
            }

            std::vector<std::uint8_t> parallel_done;
            std::vector<MatchResultCollection> parallel_collections;
            std::vector<Minimap2Params> parallel_params;

            auto& pool = bioseq::parallelism::GlobalPool::instance();
            const bool may_dispatch_parallel =
                !dispatch_params.enable_profiling
                && !selected_vulkan_execution(dispatch_params)
                && !use_scanner_pg
                && !chunked_scanner_forced
                && bioseq::parallelism::dispatch_parallel(pool.mode())
                && pool.max_concurrency() > 1
                && batch_size >= 2;
            if (may_dispatch_parallel) {
                parallel_done.assign(batch_size, 0);
                parallel_collections.resize(batch_size);
                parallel_params.resize(batch_size);

                pool.parallel_for_with_context(0, batch_size, 1,
                    bioseq::parallelism::range_body{
                    [batch_size, alphabet_code, storage_unit_byte_width,
                     query_hdr, per_row_query_hdrs, per_row_query_mode,
                     &target_hdrs, &dispatch_params, &parallel_done,
                     &parallel_collections, &parallel_params](
                            std::size_t lo, std::size_t hi) {
                        for (std::size_t i = lo; i < hi && i < batch_size; ++i) {
                            const pgseq_t* row_query_hdr =
                                per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                            const pgseq_t* target_hdr = target_hdrs[i];
                            if (row_query_hdr == nullptr || target_hdr == nullptr) {
                                continue;
                            }
                            if (row_query_hdr->alphabet_code != alphabet_code
                                || row_query_hdr->storage_unit_byte_width != storage_unit_byte_width
                                || target_hdr->alphabet_code != alphabet_code
                                || target_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                                continue;
                            }
                            if (target_hdr->bp_size > kPgSegmentationThresholdBp) {
                                continue;
                            }
                            const size_t min_bp = std::min<size_t>(
                                static_cast<size_t>(row_query_hdr->bp_size),
                                static_cast<size_t>(target_hdr->bp_size));
                            Minimap2Params params = apply_size_heuristic(dispatch_params, min_bp);
                            parallel_collections[i] =
#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_HELPER)
                                BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION(
                                    row_query_hdr,
                                    target_hdr,
                                    static_cast<sequence_size_t>(target_hdr->bp_size),
                                    params,
                                    false);
#else
                                process_inmemory_row_typed<S>(
                                    row_query_hdr,
                                    target_hdr,
                                    static_cast<sequence_size_t>(target_hdr->bp_size),
                                    params,
                                    false);
#endif
                            parallel_params[i] = params;
                            parallel_done[i] = 1;
                        }
                    }});
            }

            for (size_t i = 0; i < batch_size; ++i) {
                // DOC0276 — cancellation point at the per-row boundary. The
                // native aligner itself is NO_POSTGRES and cannot call
                // CHECK_FOR_INTERRUPTS; this lets a multi-row mmsearch honour
                // pg_cancel_backend between rows, and (with the storage-layer
                // check in fetch_payload) bounds how long a cancel waits.
                CHECK_FOR_INTERRUPTS();
                if (i < parallel_done.size() && parallel_done[i] != 0) {
                    const auto requested_source =
                        requested_target_index_source(scanner_pg_ctx);
                    const bool record_this_target =
                        record_target_operational_outcomes
                        && (scanner_pg_ctx == nullptr
                            || !scanner_pg_ctx->shared_target_batch
                            || !shared_target_outcome_recorded);
                    if (record_this_target) {
                        record_target_row(stats);
                        record_target_index_outcome(
                            stats,
                            requested_source,
                            ::bioseq::pipeline_stats::TargetIndexSource::recomputed);
						shared_target_outcome_recorded = true;
                    }
                    record_unattested_job(
                        stats,
                        parallel_params[i],
                        requested_source,
                        ::bioseq::pipeline_stats::TargetIndexSource::recomputed,
                        scanner_pg_ctx,
                        i,
                        per_row_query_mode,
                        broadcast_query_ordinal,
                        &parallel_collections[i],
                        /*consumed_shared_cached_query_proof=*/false,
                        cached_query_minimizer_proof_operation_accounted);
                    const sequence_size_t target_bp_size =
                        static_cast<sequence_size_t>(target_hdrs[i]->bp_size);
                    const std::size_t match_count = parallel_collections[i].size();
                    bioseq_log<DBG>("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es) [dispatch-parallel]",
                            static_cast<int>(query_ids[i]),
                            target_bp_size,
                            parallel_params[i].kmer_size,
                            parallel_params[i].window_size,
                            parallel_params[i].min_chain_score,
                            parallel_params[i].min_seed_count,
                            false, match_count);
                    if (!parallel_collections[i].empty()) {
                        results[query_ids[i]] = std::move(parallel_collections[i].results);
                    }
                    continue;
                }
                const pgseq_t* row_query_hdr =
                    per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                if (row_query_hdr == nullptr) {
                    continue;
                }
                if (row_query_hdr->alphabet_code != alphabet_code
                    || row_query_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                    bioseq_log<DBG>("[bioseq] pg: query row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(row_query_hdr->alphabet_code),
                            static_cast<unsigned>(row_query_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                const pgseq_t* target_hdr = target_hdrs[i];
                // DOC0274 — when use_scanner_pg=true AND a locator was captured
                // for this row (e.g. a stub whose materialised varlena would
                // exceed MaxAllocSize, so hydration was skipped), we still have
                // enough info to dispatch via scanner_pg's PG-backed mode. Only
                // skip when NEITHER an inline target nor a locator is available.
                const bool has_locator_for_pg =
                    use_scanner_pg
                    && scanner_pg_ctx->target_locators[i].has_value();
                if (target_hdr == nullptr && !has_locator_for_pg) {
                    continue;  // NULL seq column in this row → no match
                }
                if (target_hdr != nullptr
                    && (target_hdr->alphabet_code != alphabet_code
                        || target_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
                    /*
                     * PG's typmod coercion should have rejected the
                     * INSERT if the stored typmod didn't match the
                     * column typmod; getting here means someone
                     * wrote raw bytes via pg_catalog mischief or a
                     * future cross-typmod compare path. Skip rather
                     * than crash and let the tuple's mmsearch
                     * qualify as FALSE.
                     */
                    bioseq_log<DBG>("[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(target_hdr->alphabet_code),
                            static_cast<unsigned>(target_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                // Construct compact_sequence_range only when target_hdr is
                // present. With a PG-backed locator we don't materialise the
                // full payload — scanner_pg streams fragment-by-fragment. Use
                // the locator's total_bp_size for the heuristic-relevant
                // min_bp comparison.
                const sequence_size_t target_bp_size = target_hdr != nullptr
                    ? static_cast<sequence_size_t>(target_hdr->bp_size)
                    : scanner_pg_ctx->target_locators[i]->total_bp_size;

                const size_t min_bp = std::min<size_t>(
                    static_cast<size_t>(row_query_hdr->bp_size),
                    static_cast<size_t>(target_bp_size));

                Minimap2Params params = apply_size_heuristic(dispatch_params, min_bp);
                MatchResultCollection collection;
                bool consumed_shared_cached_query_proof = false;
                {
                    if (use_scanner_pg) {
                        /*
                         * Build the query compact_sequence_range inside the
                         * row loop so per-row mmsearch can batch-dispatch
                         * without broadcasting one row's query to the rest of
                         * the batch.
                         */
                        compact_sequence_range<S> query_seq{*row_query_hdr};
#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_SCANNER_HELPER)
                        collection = BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION(
                            query_seq,
                            target_hdr,
                            i,
                            target_bp_size,
                            min_bp,
                            dispatch_params,
                            params,
                            per_row_query_mode,
                            cached_query_minimizers,
                            cached_query_minimizer_proof,
                            consumed_shared_cached_query_proof,
                            alphabet_code,
                            storage_unit_byte_width,
                            scanner_pg_ctx,
                            pool);
#else
                        collection = process_scanner_pg_row_typed<S>(
                            query_seq,
                            target_hdr,
                            i,
                            target_bp_size,
                            min_bp,
                            dispatch_params,
                            params,
                            per_row_query_mode,
                            cached_query_minimizers,
                            cached_query_minimizer_proof,
                            consumed_shared_cached_query_proof,
                            alphabet_code,
                            storage_unit_byte_width,
                            scanner_pg_ctx,
                            pool);
#endif
                    } else if (target_hdr != nullptr) {
                        collection =
#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_HELPER)
                            BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION(
                                row_query_hdr,
                                target_hdr,
                                target_bp_size,
                                params,
                                chunked_scanner_forced);
#else
                            process_inmemory_row_typed<S>(
                                row_query_hdr,
                                target_hdr,
                                target_bp_size,
                                params,
                                chunked_scanner_forced);
#endif
                    } else if (scanner_pg_ctx != nullptr
                               && scanner_pg_ctx->target_locators[i].has_value()) {
                        // DOC0274 (round-2 revert): inmemory_forced=ON cannot satisfy its
                        // force-baseline semantic on > MaxAllocSize stubs; raise.
                        bioseq_log<ERR, PG>(
                            "bioseq.test_force_inmemory_scanner=on: row {} target stub "
                            "({} bp) exceeds MaxAllocSize and cannot be materialised "
                            "for the Phase B baseline. Either set the GUC to off (use "
                            "the Phase C scanner_pg path which streams fragments) or "
                            "wait for Phase D D-VARLENA. See DOC0267.",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned long long>(target_bp_size));
                    } else {
                        bioseq_log<DBG>("[bioseq] pg: row {} has no inline target and no scanner_pg locator; treating as WHERE FALSE",
                                static_cast<int>(query_ids[i]));
                        continue;
                    }
                }

                const auto target_index_source = use_scanner_pg
                    ? scanner_pg_ctx->target_index_sources[i]
                    : ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
                const auto requested_source =
                    requested_target_index_source(scanner_pg_ctx);
                const bool record_this_target =
                    record_target_operational_outcomes
                    && (scanner_pg_ctx == nullptr
                        || !scanner_pg_ctx->shared_target_batch
                        || !shared_target_outcome_recorded);
                if (record_this_target) {
                    record_target_row(stats);
                    record_target_index_outcome(
                        stats, requested_source, target_index_source);
					shared_target_outcome_recorded = true;
                }
                record_unattested_job(
                    stats,
                    params,
                    requested_source,
                    target_index_source,
                    scanner_pg_ctx,
                    i,
                    per_row_query_mode,
                    broadcast_query_ordinal,
                    &collection,
                    consumed_shared_cached_query_proof,
                    cached_query_minimizer_proof_operation_accounted);

                bioseq_log<DBG>("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
                        static_cast<int>(query_ids[i]),
                        target_bp_size,
                        params.kmer_size,
                        params.window_size,
                        params.min_chain_score,
                        params.min_seed_count,
                        target_index_source
                            == ::bioseq::pipeline_stats::TargetIndexSource::persisted,
                        collection.size());
                if (!collection.empty()) {
                    results[query_ids[i]] = std::move(collection.results);
                }
            }
    return results;
}
#endif

} // namespace

#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET must name the concrete sequence alphabet"
#endif
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH must name the concrete storage-unit byte width"
#endif

#if defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION must name the concrete scanner helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_SCANNER_FUNCTION(
    compact_sequence_range<compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>>& query_seq,
    const pgseq_t* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    const Minimap2Params& dispatch_params,
    const Minimap2Params& params,
    const bool per_row_query_mode,
    std::optional<std::vector<MinimapSketchData>>& cached_query_minimizers,
    std::optional<pipe::MinimizerRangeExecutionProof>&
        cached_query_minimizer_proof,
    bool& consumed_shared_cached_query_proof,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx,
    bioseq::parallelism::GlobalPool& pool)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_scanner_pg_row_typed<S>(
        query_seq,
        target_hdr,
        i,
        target_bp_size,
        min_bp,
        dispatch_params,
        params,
        per_row_query_mode,
        cached_query_minimizers,
        cached_query_minimizer_proof,
        consumed_shared_cached_query_proof,
        alphabet_code,
        storage_unit_byte_width,
        scanner_pg_ctx,
        pool);
}
#elif defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION must name the concrete in-memory helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const sequence_size_t target_bp_size,
    const Minimap2Params& params,
    const bool chunked_scanner_forced)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_inmemory_row_typed<S>(
        row_query_hdr,
        target_hdr,
        target_bp_size,
        params,
        chunked_scanner_forced);
}
#elif defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION must name the concrete standard in-memory helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_inmemory_standard_row_typed<S>(
        row_query_hdr,
        target_hdr,
        params);
}
#elif defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION must name the concrete segmented in-memory helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_inmemory_segmented_row_typed<S>(
        row_query_hdr,
        target_hdr,
        params);
}
#elif defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION must name the concrete chunked in-memory helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_inmemory_chunked_row_typed<S>(
        row_query_hdr,
        target_hdr,
        params);
}
#elif defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_ONLY)
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION must name the concrete chunked segmented in-memory helper"
#endif

[[nodiscard]] MatchResultCollection
BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION(
    const pgseq_t* row_query_hdr,
    const pgseq_t* target_hdr,
    const Minimap2Params& params)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_inmemory_chunked_segmented_row_typed<S>(
        row_query_hdr,
        target_hdr,
        params);
}
#else
#ifndef BIOSEQ_DISPATCH_PROCESSOR_TYPED_FUNCTION
#error "BIOSEQ_DISPATCH_PROCESSOR_TYPED_FUNCTION must name the concrete typed dispatch function"
#endif

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>>
BIOSEQ_DISPATCH_PROCESSOR_TYPED_FUNCTION(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    const BatchInvocationRole invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx)
{
    using S = compact_sequence_stdc_instance<
        pgseq_t,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET,
        BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH>;
    return process_sequences_typed<S>(
        batch_size,
        alphabet_code,
        storage_unit_byte_width,
        query_hdr,
        per_row_query_hdrs,
        target_hdrs,
        query_ids,
        base_params,
        invocation_role,
        broadcast_query_ordinal,
        stats,
        scanner_pg_ctx);
}
#endif

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
