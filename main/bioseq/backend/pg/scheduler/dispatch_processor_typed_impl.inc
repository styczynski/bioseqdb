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

/// Arms before fragment-anchor preparation and completes the scratch wave only
/// after the synchronous prepare/consumer scope has unwound. The scratch API is
/// idempotent/noexcept, so preparation and alignment failures both leave the
/// reusable planner clean.
class FragmentAnchorWaveCompletion final {
private:
    bioseq::indexing::QueryWaveScratch* scratch_ = nullptr;

public:
    explicit FragmentAnchorWaveCompletion(
            bioseq::indexing::QueryWaveScratch& scratch) noexcept
        : scratch_(&scratch) {}

    ~FragmentAnchorWaveCompletion() noexcept {
        scratch_->complete_wave_after_join();
    }

    FragmentAnchorWaveCompletion(
        const FragmentAnchorWaveCompletion&) = delete;
    FragmentAnchorWaveCompletion& operator=(
        const FragmentAnchorWaveCompletion&) = delete;
};

constexpr std::string_view kMinimizersPerFragmentIndex =
    bioseq::indexing::MinimizerHashmapFragmentIndex::index_name;
constexpr std::string_view kMinimizersPerSequenceIndex =
    bioseq::indexing::index_descriptor<
        bioseq::indexing::MinimizerSequenceMergedIndex>::index_name;
constexpr sequence_size_t kPgSegmentationThresholdBp = 256'000'000;
constexpr sequence_size_t kOpportunisticIndexMinBp = 4096;

// GetConfigOptionByName returns a palloc-owned string and can ereport.  Keep the
// PostgreSQL call and release in separate PG leaves; copy only after the getter
// leaf has restored PG state so the owning std::string always unwinds normally.
[[nodiscard]] inline std::optional<std::string> pg_guc_value_as_cpp(
		const char* name) {
	char* raw_value = bioseq::pg_utils::invoke_pg_as_cpp(
		[name] { return GetConfigOptionByName(name, nullptr, true); });
	if (raw_value == nullptr) {
		return std::nullopt;
	}

	std::string value;
	try {
		value.assign(raw_value);
	} catch (...) {
		bioseq::pg_utils::invoke_pg_as_cpp(
			[raw_value] { pfree(raw_value); });
		throw;
	}
	bioseq::pg_utils::invoke_pg_as_cpp(
		[raw_value] { pfree(raw_value); });
	return value;
}

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

    const std::string sql = fmt::format(
        "SELECT index_name, enabled, update_on_insert "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
        "AND column_attno={}",
        static_cast<unsigned>(ctx.user_relid),
        static_cast<unsigned>(ctx.main_relfilenode),
        static_cast<int>(ctx.mmsearch_seq_attno));

    struct SpiPolicyReadState {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        bool table_present = false;
        const char* semantic_error = nullptr;
    } spi;

    bioseq::pg_utils::invoke_pg_as_cpp(
        [&spi, &sql, &minimizers, &enabled, &update_on_insert] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) {
                return;
            }

            spi.execute_status = SPI_execute(sql.c_str(), true, 0);
            if (spi.execute_status == SPI_OK_SELECT) {
                spi.table_present = SPI_tuptable != nullptr;
                if (!spi.table_present) {
                    spi.semantic_error =
                        "deferred-persist policy result has no tuple table";
                } else {
                    const std::uint64_t row_count =
                        static_cast<std::uint64_t>(SPI_processed);
                    for (std::uint64_t row = 0; row < row_count; ++row) {
                        char* const name_value = SPI_getvalue(
                            SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1);
                        if (name_value == nullptr) {
                            spi.semantic_error =
                                "deferred-persist policy index_name is NULL";
                            break;
                        }

                        bool enabled_is_null = false;
                        const Datum enabled_value = SPI_getbinval(
                            SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2,
                            &enabled_is_null);
                        bool update_is_null = false;
                        const Datum update_value = SPI_getbinval(
                            SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 3,
                            &update_is_null);
                        if (enabled_is_null || update_is_null) {
                            spi.semantic_error =
                                "deferred-persist policy boolean is NULL";
                        } else {
                            const std::string_view name{name_value};
                            for (std::size_t i = 0; i < minimizers.size(); ++i) {
                                if (minimizers[i] == name) {
                                    enabled[i] = DatumGetBool(enabled_value);
                                    update_on_insert[i] = DatumGetBool(update_value);
                                    break;
                                }
                            }
                        }
                        pfree(name_value);
                        if (spi.semantic_error != nullptr) {
                            break;
                        }
                    }
                }
            }

            spi.finish_status = SPI_finish();
        });

    if (spi.connect_status != SPI_OK_CONNECT) {
        bioseq::storage::indexing::active_read_policy_error_as_cpp(
            "deferred-persist SPI_connect failed",
            fmt::format("SPI rc={}", spi.connect_status));
    }
    if (spi.finish_status != SPI_OK_FINISH) {
        bioseq::storage::indexing::active_read_policy_error_as_cpp(
            "deferred-persist SPI_finish failed",
            fmt::format("SPI rc={}", spi.finish_status));
    }
    if (spi.execute_status != SPI_OK_SELECT) {
        bioseq::storage::indexing::active_read_policy_error_as_cpp(
            "deferred-persist policy query failed",
            fmt::format("SPI rc={}", spi.execute_status));
    }
    if (!spi.table_present) {
        bioseq::storage::indexing::active_read_policy_error_as_cpp(
            "deferred-persist policy result is invalid",
            "deferred-persist policy result has no tuple table");
    }
    if (spi.semantic_error != nullptr) {
        bioseq::storage::indexing::active_read_policy_error_as_cpp(
            "deferred-persist policy result is invalid", spi.semantic_error);
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
    return pipe::vulkan_components_requested(params);
}

[[nodiscard]] inline bool selected_vulkan_accelerator(
        const Minimap2Params& params) noexcept {
    return pipe::vulkan_accelerator_requested(params);
}

[[nodiscard]] inline bool selected_vulkan_coordinate_prereqs_active(
        const Minimap2Params& params) noexcept {
    return pipe::complete_all_gpu_tuple_selected(params);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::ExecutedBackend
backend_from_selected_params(const Minimap2Params& params) noexcept {
    const bool full_vulkan_pipeline =
        pipe::complete_all_gpu_tuple_selected(params);
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

[[nodiscard]] inline ::bioseq::pipeline_stats::SimdChainingPolicyIdentity
requested_simd_chaining_policy_identity(
        const Minimap2Params& params) noexcept {
    using Identity =
        ::bioseq::pipeline_stats::SimdChainingPolicyIdentity;
    if (params.chaining_algorithm != ChainingAlgorithmType::SIMD_MM2) {
        return Identity::none;
    }
    switch (params.simd_chaining_policy) {
        case SimdChainingPolicy::exact_visited:
            return Identity::exact_visited;
        case SimdChainingPolicy::bounded_batch_approximate:
            return Identity::bounded_batch_approximate;
    }
    return Identity::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::SimdChainingPolicyIdentity
observed_simd_chaining_policy_identity(
        const MatchResultCollection* execution_proof) noexcept {
    using Identity =
        ::bioseq::pipeline_stats::SimdChainingPolicyIdentity;
    if (execution_proof == nullptr
        || !execution_proof->simd_chaining_execution_proof_present) {
        return Identity::none;
    }
    const auto& policy =
        execution_proof->simd_chaining_execution_proof.policy;
    // Identity answers only which named policy emitted operation calls.
    // Rejections, saturation, forbidden scalar work, and missing vector work
    // are retained as separate fail-closed evidence below; folding them into
    // identity would mislabel a rejected exact (or bounded) execution as a
    // mixed-policy execution and hide the actual reason for rejection.
    const bool exact = policy.exact_visited_calls != 0;
    const bool bounded = policy.bounded_batch_approximate_calls != 0;
    if (exact == bounded) {
        // Neither identity, or an impossible proof accepting both identities,
        // is not a policy absence. Preserve it as mixed-invalid evidence.
        return Identity::mixed_invalid;
    }
    return exact ? Identity::exact_visited
                 : Identity::bounded_batch_approximate;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::SimdAlignmentAlgorithmIdentity
requested_simd_alignment_algorithm_identity(
        const Minimap2Params& params) noexcept {
    using Identity =
        ::bioseq::pipeline_stats::SimdAlignmentAlgorithmIdentity;
    return backend_from_selected_params(params)
            == ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu
        ? Identity::block_aligner
        : Identity::none;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::SimdAlignmentAlgorithmIdentity
observed_simd_alignment_algorithm_identity(
        const MatchResultCollection* execution_proof) noexcept {
    using Identity =
        ::bioseq::pipeline_stats::SimdAlignmentAlgorithmIdentity;
    if (execution_proof == nullptr) return Identity::none;

    const bool block_present =
        execution_proof->block_aligner_execution_proof_present;
    const auto& block = execution_proof->block_aligner_execution_proof;
    const auto& scalar = execution_proof->scalar_pipeline_execution_proof;
    const bool scalar_alignment_observed =
        execution_proof->scalar_pipeline_execution_proof_present
        && (scalar.ksw2_left_calls != 0
            || scalar.ksw2_gap_calls != 0
            || scalar.ksw2_right_calls != 0);
    const bool forbidden_alignment_observed = scalar_alignment_observed
        || execution_proof->ksw2_batched_execution_proof_present
        || (block_present
            && (block.forbidden_ksw2_calls != 0
                || block.forbidden_scalar_biological_fallbacks != 0));
    if (block_present && forbidden_alignment_observed) {
        return Identity::mixed_invalid;
    }
    return block_present ? Identity::block_aligner : Identity::none;
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

[[nodiscard]] inline std::uint64_t dispatch_host_now_ns() noexcept {
    const auto now = bioseq::instr::clock::now_ns();
    return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
}

[[nodiscard]] inline std::uint64_t elapsed_dispatch_host_ns(
        const std::uint64_t started_ns) noexcept {
    const std::uint64_t finished_ns = dispatch_host_now_ns();
    const std::uint64_t elapsed_ns = finished_ns >= started_ns
        ? finished_ns - started_ns : 0u;
    return elapsed_ns > 0 ? elapsed_ns : 1u;
}

inline void record_block_aligner_execution_evidence(
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const MatchResultCollection* execution_proof) noexcept {
    if (execution_proof == nullptr
        || !execution_proof->block_aligner_execution_proof_present) {
        return;
    }
    const auto& proof = execution_proof->block_aligner_execution_proof;
    ::bioseq::pipeline_stats::BlockAlignerExecutionEvidence evidence{};
    evidence.present = true;
    evidence.accepted = proof.accepted();
    evidence.executed_calls = proof.executed_calls();
    evidence.nonempty_calls = proof.nonempty_calls;
    evidence.nonempty_calls_with_simd = proof.nonempty_calls_with_simd;
    evidence.simd_dp_vector_batches = proof.simd_dp_vector_batches;
    evidence.simd_dp_cells = proof.simd_dp_cells;
    evidence.forbidden_ksw2_calls = proof.forbidden_ksw2_calls;
    evidence.forbidden_scalar_biological_fallbacks =
        proof.forbidden_scalar_biological_fallbacks;
    evidence.saturated = proof.saturated;
    stats.operational.stats_saturated =
        stats.operational.block_aligner_execution_evidence.merge(evidence)
        || stats.operational.stats_saturated;
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
    if (execution_proof != nullptr) {
        if (execution_proof->prefilter_execution_proof_present) {
            stats.operational.record_prefilter_execution_proof(
                execution_proof->prefilter_execution_proof);
        } else if (execution_proof->prefilter_execution_proof
                   != decltype(execution_proof->prefilter_execution_proof){}) {
            auto malformed_prefilter_proof =
                execution_proof->prefilter_execution_proof;
            malformed_prefilter_proof.saturated = true;
            stats.operational.record_prefilter_execution_proof(
                malformed_prefilter_proof);
        }
        // CPU/SIMD stages write into the operation-local result collection;
        // consume that delta once here, after any parallel row has joined.
        // ChainStage is intentionally inclusive, so chain_backtrack remains
        // zero. The left_extension timing slot likewise owns the coarse
        // alignment-through-result interval; gap/right remain available for a
        // later measured split. These owners never overlap.
        // query_sketch is only the per-operation sketch recorded below or by
        // MinimizerStage; the BatchJoin query-union preparation that precedes
        // persisted payload delivery has a different lifetime owner and must
        // be published separately rather than charged here twice.
        using Stage = ::bioseq::pipeline_stats::PipelineStage;
        add_stage_host_time(
            stats, Stage::query_sketch,
            execution_proof->query_sketch_host_wall_ns);
        add_stage_host_time(
            stats, Stage::persisted_index_decode,
            execution_proof->persisted_index_decode_host_wall_ns);
        add_stage_host_time(
            stats, Stage::seed_lookup,
            execution_proof->seed_lookup_host_wall_ns);
        add_stage_host_time(
            stats, Stage::chain_score,
            execution_proof->chain_score_host_wall_ns);
        add_stage_host_time(
            stats, Stage::region_generation,
            execution_proof->region_generation_host_wall_ns);
        add_stage_host_time(
            stats, Stage::left_extension,
            execution_proof->alignment_result_host_wall_ns);
        stats.operational.stats_saturated =
            stats.operational.stats_saturated
            || execution_proof->host_stage_timing_saturated;
        if (execution_proof->fragment_native_query_evidence_present) {
            stats.operational.fragment_native_query_evidence.merge(
                execution_proof->fragment_native_query_evidence);
            stats.operational.stats_saturated =
                stats.operational.stats_saturated
                || stats.operational.fragment_native_query_evidence.saturated;
        }
    }
    ::bioseq::pipeline_stats::PipelineJobExecutionStats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_simd_chaining_policy =
        requested_simd_chaining_policy_identity(params);
    job.observed_simd_chaining_policy =
        observed_simd_chaining_policy_identity(execution_proof);
    job.requested_simd_alignment_algorithm =
        requested_simd_alignment_algorithm_identity(params);
    job.observed_simd_alignment_algorithm =
        observed_simd_alignment_algorithm_identity(execution_proof);
    const bool selected_all_simd =
        job.requested_backend
            == ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu;
    const bool selected_all_scalar =
        job.requested_backend
            == ::bioseq::pipeline_stats::ExecutedBackend::scalar_cpu;
    const bool require_target_sketch =
        actual_source
            == ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    const bool accepted_all_simd = selected_all_simd
        && execution_proof != nullptr
        && execution_proof->all_simd_execution_accepted(
            params.minimizer_algorithm,
            params.simd_chaining_policy,
            require_target_sketch,
            /*require_nonempty_vector_work=*/true);
    const bool accepted_all_scalar = selected_all_scalar
        && execution_proof != nullptr
        && execution_proof->all_scalar_execution_accepted(
            require_target_sketch,
            /*require_alignment=*/params.do_cigar);
    if (execution_proof != nullptr
        && execution_proof->simd_chaining_execution_proof_present) {
        const auto& policy =
            execution_proof->simd_chaining_execution_proof.policy;
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_exact_policy_calls,
            policy.exact_visited_calls);
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_bounded_policy_calls,
            policy.bounded_batch_approximate_calls);
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_bounded_score_batches,
            policy.bounded_score_batches);
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_bounded_cutoff_stops,
            policy.bounded_cutoff_stops);
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_bounded_candidates_skipped,
            policy.bounded_candidates_skipped);
        add_operational_counter(
            stats,
            stats.operational.simd_chaining_policy_rejections,
            policy.policy_rejections);
        stats.operational.simd_chaining_policy_evidence_saturated =
            stats.operational.simd_chaining_policy_evidence_saturated
            || policy.saturated;
    }
    record_block_aligner_execution_evidence(stats, execution_proof);
    const auto record_exact_nthash_vector_activity = [&stats](
            const auto& proof) noexcept {
        // Continuation batches and whole-range resident vector_stream calls
        // remain separate evidence. A resident call is authentic vector work,
        // but it is not one continuation batch.
        add_operational_counter(
            stats,
            stats.operational.nthash_vector_batches,
            proof.vector_batches);
        add_operational_counter(
            stats,
            stats.operational.nthash_resident_vector_stream_kernel_calls,
            proof.resident_vector_stream_kernel_calls);
    };
    // A broadcast persisted-query sketch is one physical operation shared by
    // logical jobs. Account it at its first proof-valid consumer even when
    // that job later has no chain/alignment; downstream route attestation is
    // a separate question. Recompute rows cannot enter this block because
    // their consumed_shared_cached_query_proof flag is false.
    const bool accepted_shared_query_sketch =
        consumed_shared_cached_query_proof
        && !cached_query_minimizer_proof_operation_accounted
        && execution_proof != nullptr
        && (selected_all_simd
            ? execution_proof->selected_simd_sketch_execution_accepted(
                params.minimizer_algorithm,
                /*require_target_sketch=*/false,
                /*require_nonempty_vector_work=*/false)
            : selected_all_scalar
                && execution_proof->selected_scalar_sketch_execution_accepted(
                    /*require_target_sketch=*/false,
                    /*require_nonempty_work=*/false));
    if (accepted_shared_query_sketch) {
        if (params.minimizer_algorithm
                == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
            record_exact_nthash_vector_activity(
                execution_proof->exact_simd_query_execution_proof);
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
        : accepted_all_scalar
            ? ::bioseq::pipeline_stats::ExecutedBackend::scalar_cpu
            : ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid;
    job.requested_target_index_source = requested_source;
    job.actual_target_index_source = actual_source;
	if (actual_source
			== ::bioseq::pipeline_stats::TargetIndexSource::persisted
		&& scanner_pg_ctx != nullptr) {
		job.persisted_generation_id =
			scanner_pg_ctx->persisted_generation_ids[row_index];
		job.persisted_source_identity =
			scanner_pg_ctx->persisted_source_identities[row_index];
	}
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
    if (accepted_all_simd || accepted_all_scalar) {
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
        const Engine biological_engine = accepted_all_scalar
            ? Engine::scalar_cpu
            : Engine::simd_cpu;
        mark(Stage::query_sketch, biological_engine);
        if (require_target_sketch) {
            mark(Stage::target_sketch, biological_engine);
            mark(Stage::target_index_build, Engine::scalar_cpu);
		} else {
			mark(Stage::persisted_index_decode, Engine::host_io);
			mark(Stage::partition_selection, Engine::scalar_cpu);
        }
        mark(Stage::seed_lookup, Engine::scalar_cpu);
        mark(Stage::chain_score, biological_engine);
        mark(Stage::chain_backtrack, biological_engine);
        if (accepted_all_simd) {
            const auto& block = execution_proof->block_aligner_execution_proof;
            if (block.left_executed != 0) {
                mark(Stage::left_extension, Engine::simd_cpu);
            }
            if (block.gap_executed != 0) {
                mark(Stage::gap_extension, Engine::simd_cpu);
            }
            if (block.right_executed != 0) {
                mark(Stage::right_extension, Engine::simd_cpu);
            }
        } else {
            const auto& scalar =
                execution_proof->scalar_pipeline_execution_proof;
            if (scalar.ksw2_left_calls != 0) {
                mark(Stage::left_extension, Engine::scalar_cpu);
            }
            if (scalar.ksw2_gap_calls != 0) {
                mark(Stage::gap_extension, Engine::scalar_cpu);
            }
            if (scalar.ksw2_right_calls != 0) {
                mark(Stage::right_extension, Engine::scalar_cpu);
            }
        }

        if (accepted_all_simd
            && params.minimizer_algorithm
                == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
            if (!consumed_shared_cached_query_proof) {
                record_exact_nthash_vector_activity(
                    execution_proof->exact_simd_query_execution_proof);
            }
            if (execution_proof->exact_simd_target_execution_proof_present) {
                record_exact_nthash_vector_activity(
                    execution_proof->exact_simd_target_execution_proof);
            }
        } else if (accepted_all_simd
                   && params.minimizer_algorithm
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
        if (accepted_all_simd) {
            add_operational_counter(
                stats, stats.operational.simd_chain_vector_batches,
                execution_proof->simd_chaining_execution_proof
                    .score_vector_batches);
            add_operational_counter(
                stats,
                stats.operational.simd_chain_backtrack_vector_batches,
                execution_proof->simd_chaining_execution_proof
                    .backtrack_path_vector_batches);
        }
    }
    // Unsupported tuples and incomplete scalar/SIMD paths deliberately remain
    // unattested. Persisted identity remains fail-closed until its reader
    // publishes the generation proof.
    stats.merge_job(job);
}

/// Consume one runtime-sealed persisted Shape-V5 coordinate proof into a local
/// job value.  Nothing caller-visible is mutated here: a missing, tampered or
/// repeated consume burns only the runtime record and returns no staged job.
/// The accepted operation authority supplies the immutable GPU environment
/// after every job and the complete causal receipt union have been validated.
[[nodiscard]] inline std::optional<
    ::bioseq::pipeline_stats::PipelineJobExecutionStats>
stage_persisted_shape_v5_coordinate_job(
        const Minimap2Params& params,
        const ::bioseq::pipeline_stats::TargetIndexSource requested_source,
        bioseq::gpu::vulkan::PersistedShapeV5CoordinateOperationProof& proof,
        const bioseq::gpu::vulkan::
            PersistedShapeV5CoordinateJobExpectation& expectation) noexcept {
    ::bioseq::pipeline_stats::PipelineJobExecutionStats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_target_index_source = requested_source;
    const auto evidence = proof.consume(expectation);
    const bool accepted =
        job.requested_backend
            == ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
        && selected_vulkan_execution(params)
        && !params.do_cigar
        && pipe::phase_c_coordinate_only_active(params)
        && pipe::phase_c_index_build_active(params)
        && requested_source
            == ::bioseq::pipeline_stats::TargetIndexSource::persisted
        && evidence.has_value()
        && evidence->accepted();
    if (!accepted) return std::nullopt;
    job.actual_backend = ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu;
    job.actual_target_index_source =
        ::bioseq::pipeline_stats::TargetIndexSource::persisted;
    job.persisted_generation_id = evidence->audit().generation_id;
    job.persisted_source_identity = evidence->audit().source_identity;
    job.logical_target_row = expectation.logical_target_row;
    job.query_ordinal = expectation.logical_query_ordinal;
    const auto mark = [&job](
            const ::bioseq::pipeline_stats::PipelineStage stage,
            const ::bioseq::pipeline_stats::ExecutionEngine engine) {
        auto& coverage = job.stages.coverage[
            ::bioseq::pipeline_stats::stage_index(stage)];
        coverage.eligible_jobs = 1u;
        coverage.executed_jobs[
            ::bioseq::pipeline_stats::engine_index(engine)] = 1u;
    };
    using Stage = ::bioseq::pipeline_stats::PipelineStage;
    using Engine = ::bioseq::pipeline_stats::ExecutionEngine;
    mark(Stage::query_sketch, Engine::vulkan_gpu);
    mark(Stage::persisted_index_decode, Engine::host_io);
    if (evidence->audit().query_record.minimizer_count != 0u) {
        mark(Stage::partition_selection, Engine::vulkan_gpu);
        mark(Stage::seed_lookup, Engine::vulkan_gpu);
    }
    if (evidence->audit().range_count != 0u) {
        mark(Stage::chain_score, Engine::vulkan_gpu);
        mark(Stage::chain_backtrack, Engine::vulkan_gpu);
        mark(Stage::region_generation, Engine::vulkan_gpu);
    }
    return job;
}

/// Copy one runtime cache audit into the pointer-free SQL ledger wire.
[[nodiscard]] inline ::bioseq::pipeline_stats::
PersistedGpuCacheMetricsEvidence
persisted_gpu_cache_metrics_evidence(
        const bioseq::gpu::vulkan::RuntimeGpuCacheMetricsAudit& source)
        noexcept {
    return {
        .token_identity = source.token_identity,
        .pipeline_cache_loads = source.gpu_pipeline_cache_loads,
        .pipeline_cache_seed_bytes =
            source.gpu_pipeline_cache_seed_bytes,
        .pipeline_cache_load_ns = source.gpu_pipeline_cache_load_ns,
        .pipeline_creates = source.gpu_pipeline_creates,
        .pipeline_creation_ns = source.gpu_pipeline_creation_ns,
    };
}

template <std::size_t Capacity>
[[nodiscard]] inline std::uint32_t copy_persisted_gpu_evidence_text(
        std::array<char, Capacity>& destination,
        const std::string_view source) noexcept {
    if (source.empty() || source.size() > destination.size()
        || source.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return 0u;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    return static_cast<std::uint32_t>(source.size());
}

/// Copy the pointer/path/handle-free process qualification into the admitted
/// SQL ledger. An oversized helper identity becomes an invalid zero-length
/// field instead of allocating or truncating provenance.
[[nodiscard]] inline ::bioseq::pipeline_stats::
PersistedGpuCacheQualificationEvidence
persisted_gpu_cache_qualification_evidence(
        const bioseq::gpu::vulkan::
            RuntimeMetalArchiveProcessQualification& source) noexcept {
    ::bioseq::pipeline_stats::PersistedGpuCacheQualificationEvidence result{
        .version = source.version,
        .identity = source.identity,
        .archive_count = source.archive_set.archive_count,
        .archive_bytes = source.archive_set.total_size,
        .archive_checksum = source.archive_set.ordered_checksum,
        .receipt_count = source.receipt_count,
        .hash_passes = source.hash_passes,
        .hashed_bytes = source.hashed_bytes,
        .private_files_created = source.private_files_created,
        .private_files_unlinked = source.private_files_unlinked,
        .live_private_paths = source.live_private_paths,
        .pipeline_filesystem_hash_passes =
            source.pipeline_filesystem_hash_passes,
        .pipeline_filesystem_hashed_bytes =
            source.pipeline_filesystem_hashed_bytes,
        .helper_protocol_version = source.helper_protocol_version,
        .helper_executable_size = source.helper_executable_size,
        .helper_executable_sha256 = source.helper_executable_sha256,
        .helper_metal_registry_id = source.helper_metal_registry_id,
        .device_uuid = source.vulkan_device_uuid,
        .api_version = source.api_version,
        .driver_version = source.driver_version,
        .vendor_id = source.vendor_id,
        .device_id = source.device_id,
        .pipeline_cache_uuid = source.pipeline_cache_uuid,
        .seed_receipt_identity = source.seed_receipt_identity,
        .seed_sha256 = source.seed_sha256,
        .seed_size = source.seed_size,
        .manifest_checksum = source.manifest_checksum,
        .saturated = source.saturated,
    };
    result.helper_build_identity_size = copy_persisted_gpu_evidence_text(
        result.helper_build_identity, source.helper_build_identity);
    result.helper_source_size = copy_persisted_gpu_evidence_text(
        result.helper_source, source.helper_source);
    return result;
}

/// Copy one full-stage runtime job preview into the admitted ledger slot.
[[nodiscard]] inline ::bioseq::pipeline_stats::PersistedGpuJobEvidence
persisted_gpu_job_evidence(
        const bioseq::gpu::vulkan::
            PersistedShapeV5CoordinateJobEvidencePreview& source) noexcept {
    const auto& audit = source.audit;
    const auto& coordinate = audit.result;
    const std::array<std::uint64_t, 4> submit_wait_identities{
        audit.count_submit_wait_identity,
        audit.emit_submit_wait_identity,
        audit.sort_range_submit_wait_identity,
        audit.chain_coordinate_submit_wait_identity,
    };
    ::bioseq::pipeline_stats::PersistedGpuJobEvidence result{
        .logical_target_row = audit.logical_target_row,
        .logical_query_ordinal = audit.logical_query_ordinal,
        .query_index = audit.query_index,
        .query_length = audit.query_record.query_len,
        .target_length = audit.target_len,
        .query_minimizer_count = audit.query_record.minimizer_count,
        .query_tile_count = audit.query_tile_count,
        .query_count_dispatches = audit.query_count_dispatches,
        .query_scan_dispatches = audit.query_scan_dispatches,
        .query_emit_dispatches = audit.query_emit_dispatches,
        .query_completed_roundtrips = audit.query_completed_roundtrips,
        .direct_count_dispatches = audit.direct_count_dispatches,
        .direct_scan_dispatches = audit.direct_scan_dispatches,
        .anchor_count = audit.anchor_count,
        .direct_emit_dispatches = audit.direct_emit_dispatches,
        .sort_operations = audit.sort_operations,
        .range_build_dispatches = audit.range_build_dispatches,
        .range_count = audit.range_count,
        .chain_dispatches = audit.chain_dispatches,
        .chain_backtrack_dispatches = audit.chain_backtrack_dispatches,
        .coordinate_dispatches = audit.coordinate_dispatches,
        .count_status_checks = audit.count_status_checks,
        .emit_status_checks = audit.emit_status_checks,
        .status_or = audit.status_or,
        .result_readbacks = audit.result_readbacks,
        .coordinate_vote_dispatches = audit.coordinate_vote_dispatches,
        .cpu_or_simd_biological_calls =
            audit.cpu_or_simd_biological_calls,
        .recompute_calls = audit.recompute_calls,
        .fallback_calls = audit.fallback_calls,
        .coordinate = {
            .query_start = coordinate.query_start,
            .query_end = coordinate.query_end,
            .target_start = coordinate.target_start,
            .target_end = coordinate.target_end,
            .match_length = coordinate.match_length,
            .block_length = coordinate.block_length,
            .chain_score = coordinate.chain_score,
            .seed_count = coordinate.seed_count,
            .is_reverse = coordinate.is_reverse != 0u,
            .valid = coordinate.valid != 0u,
        },
    };
    for (std::size_t i = 0u; i < result.submissions.size(); ++i) {
        result.submissions[i] = {
            .operation_identity =
                source.shape_pipeline_operation_identity,
            .ordinal = source.shape_submission_ordinals[i],
            .submit_wait_identity = submit_wait_identities[i],
        };
    }
    return result;
}

/// Populate the fixed operation envelope from the pre-finalizer runtime view.
[[nodiscard]] inline ::bioseq::pipeline_stats::PersistedGpuOperationEvidence
persisted_gpu_operation_evidence(
        const bioseq::gpu::vulkan::
            PersistedShapeV5CoordinateOperationEvidencePreview& source,
        const ::bioseq::pipeline_stats::
            PersistedShapeV5CoordinatePublicationCounts& publication,
        const std::uint64_t generation_id,
        const std::uint64_t source_identity,
        const std::uint64_t cold_load_identity) noexcept {
    const auto& acceptance = source.acceptance;
    return {
        .operation_identity = acceptance.operation_identity,
        .publication = publication,
        .published_jobs = publication.expected_jobs,
        .publication_commits = 1u,
        .query_pipeline_receipt_count =
            acceptance.query_pipeline_receipt_count,
        .shape_pipeline_receipt_count =
            acceptance.shape_pipeline_receipt_count,
        .submission_count = acceptance.committed_submission_count,
        .query_pipeline_operation_identity =
            source.query_pipeline_operation_identity,
        .shape_pipeline_operation_identity =
            source.shape_pipeline_operation_identity,
        .query_submission_ordinals = source.query_submission_ordinals,
        .seed_receipt_identity = acceptance.seed_receipt_identity,
        .generation_id = generation_id,
        .source_identity = source_identity,
        .cold_load_identity = cold_load_identity,
        .environment = {
            .device_uuid = source.device_uuid,
            .api_version = source.api_version,
            .driver_version = source.driver_version,
            .vendor_id = source.vendor_id,
            .device_id = source.device_id,
            .pipeline_cache_uuid = source.pipeline_cache_uuid,
            .seed_sha256 = source.seed_sha256,
            .seed_size = source.seed_size,
            .manifest_checksum = source.manifest_checksum,
        },
        .cache_qualification =
            persisted_gpu_cache_qualification_evidence(
                source.cache_qualification),
        .cache_metrics = persisted_gpu_cache_metrics_evidence(
            source.maximum_cache_metrics),
    };
}

/// Conservative admission envelope for the N-job capacities that coexist
/// between persisted Shape-V5 execution and atomic SQL publication.  The
/// returned reservation covers transient proof/staging overlap; the result map
/// that leaves process_sequences_typed() remains part of the pre-existing
/// caller-owned output contract.
[[nodiscard]] inline std::uint64_t
persisted_shape_v5_publication_staging_bytes(
        const std::uint64_t job_count) noexcept {
    using StagedJob = ::bioseq::pipeline_stats::PipelineJobExecutionStats;
    using ResultMap = std::map<int32_t, std::vector<MatchResult>>;
    using ResultMapValue = typename ResultMap::value_type;

    std::uint64_t bytes_per_job = 0u;
    const auto add = [&bytes_per_job](const std::uint64_t bytes) noexcept {
        bytes_per_job = bioseq::parallelism::saturating_add(
            bytes_per_job, bytes);
    };
    add(sizeof(bioseq::gpu::vulkan::PhaseCSeedCoordinateJob));
    add(sizeof(std::size_t));
    add(sizeof(bioseq::gpu::vulkan::GpuCoordinateOnlyResultWire));
    // Runtime owns separate retained permits for proof records and both
    // causal audit-job vectors; counting them here would double-reserve them.
    add(sizeof(StagedJob));
    // At most one ordered-map node per job. Four pointers cover
    // parent/left/right plus allocator/alignment bookkeeping.
    add(sizeof(ResultMapValue));
    add(4u * sizeof(void*));
    // Repeated query ids can geometrically grow result vectors, but total
    // capacity remains below two elements per appended coordinate result.
    add(bioseq::parallelism::saturating_mul(2u, sizeof(MatchResult)));

    return bioseq::parallelism::saturating_mul(
        bioseq::parallelism::saturating_mul(job_count, bytes_per_job),
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
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

// PostgreSQL ERROR is implemented with longjmp.  Keep diagnostic formatting and
// every owning C++ object in the ordinary C++ caller, and enter PostgreSQL only
// through this primitive-capture leaf.  invoke_pg_as_cpp converts the ERROR to a
// PostgresErrorAsCpp after leaving PG_TRY, so normal C++ unwinding owns all
// destruction.
[[noreturn]] inline void raise_typed_dispatch_pg_error_as_cpp(
		const int sqlstate,
		const char* message,
		const char* detail = nullptr,
		const char* hint = nullptr) {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[sqlstate, message, detail, hint] {
			if (detail != nullptr && hint != nullptr) {
				ereport(ERROR,
					(errcode(sqlstate),
					 errmsg_internal("%s", message),
					 errdetail_internal("%s", detail),
					 errhint("%s", hint)));
			} else if (detail != nullptr) {
				ereport(ERROR,
					(errcode(sqlstate),
					 errmsg_internal("%s", message),
					 errdetail_internal("%s", detail)));
			} else if (hint != nullptr) {
				ereport(ERROR,
					(errcode(sqlstate),
					 errmsg_internal("%s", message),
					 errhint("%s", hint)));
			} else {
				ereport(ERROR,
					(errcode(sqlstate), errmsg_internal("%s", message)));
			}
		});
	pg_unreachable();
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
	const std::string detail = fmt::format(
		"index_read_requirement=require_compatible_persisted "
		"target_row={} active_index={} active_index_policy_source={} reason={}",
		static_cast<long long>(target_row),
		active_index,
		policy_source,
		reason);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
		"required compatible persisted minimizer index unavailable",
		detail.c_str(),
		"Publish and wire a validated revision-3 READY generation reader, "
		"or choose index_read_requirement=allow/require_absent; pair-sort "
		"rebuild and recompute fallbacks are forbidden.");
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
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_INTERNAL_ERROR,
		"Vulkan aligner execution failed",
		diagnostic.c_str(),
		"Selected Vulkan execution has no CPU fallback; fix the Vulkan "
		"runtime or select a non-Vulkan algorithm before dispatch.");
}

[[noreturn]] inline void raise_exact_simd_pg_error(
        std::string_view stage,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		"Exact SIMD minimizer execution is unsupported for this request",
		diagnostic.c_str(),
		"Exact SIMD has no scalar fallback; use DNA4 with a supported packed "
		"storage width and supported W/K/vector shape, or select a different "
		"algorithm tuple explicitly.");
}

[[noreturn]] inline void raise_paper_nthash_pg_error(
        std::string_view stage,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		"Paper ntHash SIMD minimizer execution is unsupported for this request",
		diagnostic.c_str(),
		"Paper ntHash SIMD has no scalar or Exact SIMD fallback; use DNA4 with "
		"packed uint32/uint64 storage, W=10, K=15, and an admissible range, or "
		"select another algorithm explicitly.");
}

[[noreturn]] inline void raise_simd_chaining_pg_error(
        std::string_view stage,
        pipe::SimdChainingStatus status,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	if (status == pipe::SimdChainingStatus::invalid_policy) {
		raise_typed_dispatch_pg_error_as_cpp(
			ERRCODE_INVALID_PARAMETER_VALUE,
			"SIMD chaining policy is invalid for this request",
			diagnostic.c_str(),
			"Select exact_visited or bounded_batch_approximate explicitly; "
			"the rejected request is not retried with exact or scalar chaining.");
	}
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		"SIMD chaining execution is unsupported for this request",
		diagnostic.c_str(),
		"The selected SIMD chaining policy has no exact or scalar fallback; use "
		"a supported SIMD target and valid parameters, or explicitly select a "
		"different chaining algorithm before dispatch.");
}

[[noreturn]] inline void raise_persisted_minimizer_lookup_pg_error(
        std::string_view stage,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_DATA_CORRUPTED,
		"Persisted minimizer lookup is incomplete or inconsistent",
		diagnostic.c_str(),
		"Rebuild the compatible persisted index generation; this path has no "
		"recompute or scalar fallback.");
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
    pipe::SimdChainingStatus simd_chaining_status =
        pipe::SimdChainingStatus::ok;
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
        simd_chaining_status = err.status();
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
			raise_vulkan_pg_error(stage, detail_view);
		case FailureKind::exact_simd:
			raise_exact_simd_pg_error(stage, detail_view);
		case FailureKind::paper_nthash:
			raise_paper_nthash_pg_error(stage, detail_view);
		case FailureKind::simd_chaining:
			raise_simd_chaining_pg_error(
				stage, simd_chaining_status, detail_view);
		case FailureKind::persisted_minimizer_lookup:
			raise_persisted_minimizer_lookup_pg_error(stage, detail_view);
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

template <class Storage, class Fn>
inline void with_checked_legacy_fragment_reader(
		Storage& storage,
		Fn&& function) {
	try {
		std::forward<Fn>(function)();
		storage.close_checked();
	} catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
		storage.abandon_after_pg_error_noexcept();
		throw;
	} catch (...) {
		try {
			storage.close_checked();
		} catch (...) {
			storage.abandon_after_pg_error_noexcept();
		}
		throw;
	}
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
		bioseq::pg_utils::invoke_pg_as_cpp([msg] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq opportunistic index: %.*s",
					static_cast<int>(msg.size()), msg.data())));
		});
		pg_unreachable();
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
template <sequence_alphabet Alphabet>
[[nodiscard]] MatchResultCollection build_and_align_transient_mps(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> lazy_query_view,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_view,
    const Minimap2Params& params,
    sequence_size_t total_bp_size,
    std::size_t target_letters_per_unit)
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

        const bool scalar_query_selected =
            mps_params.minimizer_algorithm
                == MinimizerAlgorithmType::SCALAR
            && mps_params.chaining_algorithm
                == ChainingAlgorithmType::SCALAR_MM2
            && mps_params.base_alignment_algorithm
                == BaseAlignmentAlgorithmType::KSW2;
        const auto backend = target_backend_from_params_for_recompute(mps_params);
        bioseq::indexing::ScalarMpsBuildExecutionProof
            scalar_target_build_proof{};
        bioseq::indexing::MinimizerSequenceMergedIndex<
            OpportunisticSequenceIndexStorage>::build(
                mps_store,
                target_view,
                backend,
                scalar_query_selected
                    ? &scalar_target_build_proof : nullptr);

        auto query_sketch =
            sketch_query_for_index_selection<MinimapSketchData>(
                lazy_query_view,
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
            [&mps_store, lazy_query_view, target_view, &query_sketch, &mps_params,
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
                lazy_query_view,
                target_view,
                query_sketch.minimizers,
                index,
                mps_params,
                gpu_index_cache_ptr,
                gpu_identity);
        };
        if (total_bp_size <= kPgSegmentationThresholdBp) {
            collection = run_mps_index(0, total_bp_size);
        } else {
            const std::size_t lpu = target_letters_per_unit;
            MatchResultCollection merged_collection;
            std::vector<MatchResult>& merged = merged_collection.results;
            const sequence_size_t query_len = lazy_query_view.bp_size();
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
        } else if (scalar_query_selected) {
            if (!query_sketch.scalar_execution_proof_present
                || !query_sketch.scalar_execution_proof.accepted()
                || query_sketch.exact_simd_execution_proof_present
                || query_sketch.paper_nthash_execution_proof_present
                || !scalar_target_build_proof.accepted()
                || backend
                    != bioseq::simd::HWY_NAMESPACE::
                        MinimizerSketchBackend::Scalar) {
                bioseq_log<ERR>(
                    "Transient MPS scalar build/query lacks actual execution proof");
                std::abort();
            }
            collection.record_scalar_query_minimizer_execution_proof(
                query_sketch.scalar_execution_proof);
            const ScalarMinimizerExecutionProof target_proof{
                .calls = scalar_target_build_proof.scalar_collection_calls,
                .input_symbols = scalar_target_build_proof.input_symbols,
                .saturated = scalar_target_build_proof.saturated,
            };
            if (!target_proof.accepted()) {
                bioseq_log<ERR>(
                    "Transient MPS scalar target proof conversion was rejected");
                std::abort();
            }
            collection.record_scalar_target_minimizer_execution_proof(
                target_proof);
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
    // Snapshot provider selection exactly once at job creation.  The staging
    // boundary validates this raw stable-ABI value and carries it through the
    // queue and metadata-only DSM; the background worker never rereads a GUC.
    const int requested_provider_wire_value =
        bioseq_index_payload_storage_backend;
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
            target_backend_from_params_for_recompute(params)),
        requested_provider_wire_value);
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

template <sequence_alphabet Alphabet>
[[nodiscard]] MatchResultCollection
run_recompute_with_deferred_minimizer_persist(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> lazy_query,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> resident_target,
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
            static_cast<sequence_size_t>(loc.total_bp_size),
            storage_unit_byte_width * CHAR_BIT
                / bioseq::storage::lazy_rank_stream_traits<Alphabet>::
                    logical_bits_per_symbol);

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

template <sequence_alphabet Alphabet>
struct ResidentTargetForWorkers {
    bioseq::storage::admitted_packed_region<Alphabet> owner;
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::optional<ResidentTargetForWorkers<Alphabet>>
try_materialize_target_for_segment_workers(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> target_view,
        std::uint8_t requested_width,
        std::uint64_t sequence_rowno) {
    const auto plan = bioseq::storage::plan_sequence_access(
        target_view,
        bioseq::storage::sequence_access_requirement::bounded_random_window,
        0u,
        target_view.bp_size(),
        requested_width);
    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(plan.admission_bytes);
    if (!permit) {
        bioseq_log<DBG>(
            "[bioseq] resident target: row={} target_bp={} admitted_bytes={} "
            "not admitted; using scanner_pg serial fallback",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(target_view.bp_size()),
            static_cast<unsigned long long>(plan.admission_bytes));
        return std::nullopt;
    }

    const std::uint64_t materialize_started_ns = dispatch_host_now_ns();
    auto owner = bioseq::storage::materialize_packed_region(
        target_view,
        plan.from,
        plan.to,
        requested_width,
        std::move(*permit));
    const std::uint64_t materialize_elapsed_ns =
        elapsed_dispatch_host_ns(materialize_started_ns);
    const auto& descriptor = owner.descriptor();
    const std::uint64_t packed_bytes =
        static_cast<std::uint64_t>(descriptor.unit_count)
        * descriptor.storage_unit_byte_width;

    bioseq_log<DBG>(
        "[bioseq] resident target: row={} target_bp={} packed_bytes={} "
        "materialize_ns={} materialized for segment workers",
        static_cast<unsigned long long>(sequence_rowno),
        static_cast<unsigned long long>(target_view.bp_size()),
        static_cast<unsigned long long>(packed_bytes),
        static_cast<unsigned long long>(materialize_elapsed_ns));

    return ResidentTargetForWorkers<Alphabet>{std::move(owner)};
}

#if !defined(BIOSEQ_DISPATCH_PROCESSOR_TYPED_HELPER_ONLY)
/// Compare every locator field before one resident target is shared by a
/// query-parallel batch. The comparison stays PostgreSQL-free so it can also
/// serve as a preflight proof before any worker is entered.
[[nodiscard]] constexpr bool same_dispatch_target_locator(
        const bioseq::storage::seq_locator& left,
        const bioseq::storage::seq_locator& right) noexcept {
    return left.version == right.version
        && left.bucket_idx == right.bucket_idx
        && left._pad_2 == right._pad_2
        && left.shard_idx == right.shard_idx
        && left.num_fragments == right.num_fragments
        && left._pad_4 == right._pad_4
        && left.total_bp_size == right.total_bp_size
        && left.first_sub_row_no == right.first_sub_row_no
        && left.source_epoch == right.source_epoch;
}

/// Validate the complete immutable Format-6 reader identity before it is
/// borrowed concurrently. No worker may observe a partially authenticated,
/// mixed-format, or legacy-transcoded prepared group.
[[nodiscard]] inline bool format6_parallel_reader_identity_valid(
        const BatchProcessor::CpuPreparedIndexGroup& group,
        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_format6_reader.has_value()
        || group.persisted_grouped32_reader.has_value()
		|| group.persisted_shape_v5_mapping_guard.has_value()
        || group.persisted_shape_v5_reader.has_value()
		|| group.persisted_mps_shape_v5_reader.has_value()
        || group.target_index
        || group.target_index_source
            != ::bioseq::pipeline_stats::TargetIndexSource::persisted
        || group.persisted_generation_id == 0u
        || group.persisted_source_identity == 0u
        || !group.persisted_format6_source_identity_present
        || group.persisted_format6_source_identity
            == std::array<std::byte, 16>{}
        || group.persisted_source_identity
            != format6_source_diagnostic_token(
                group.persisted_format6_source_identity)
        || !group.query_hash_union.empty()
        || group.query_hash_union.capacity() != 0u
        || group.persisted_format6_reader->target_length() != target_bp_size) {
        return false;
    }

    const auto& receipt = group.persisted_read_stats.format6;
    const auto& memory =
        group.persisted_format6_reader->memory_evidence();
    const std::uint64_t phase_peak = std::max({
        receipt.directory_phase_peak_bytes,
        receipt.control_phase_peak_bytes,
        receipt.posting_phase_peak_bytes,
    });
    return receipt.format_version
            == bioseq::indexing::kMinimizerFormat6Version
        && receipt.sessions == 1u
        && receipt.generation_id == group.persisted_generation_id
        && receipt.source_identity
            == group.persisted_format6_source_identity
        && receipt.directory_row_crc_receipts == receipt.directory_rows
        && receipt.control_row_crc_receipts == receipt.control_rows
        && receipt.posting_row_crc_receipts == receipt.posting_rows
        && receipt.control_block_crc_receipts
            == receipt.selected_block_count
        && receipt.control_semantic_receipts
            == receipt.selected_block_count
        && receipt.posting_ranges_delivered
            == receipt.posting_semantic_receipts
        && receipt.posting_range_crc_receipts
            <= receipt.posting_ranges_delivered
        && receipt.query_count == memory.query_count
        && memory.query_count
            == group.canonical_query_hash_union_u32.size()
        && receipt.accepted_hash_count == memory.accepted_hash_count
        && receipt.retained_position_count
            == memory.retained_position_count
        && receipt.peak_admitted_bytes == phase_peak
        && receipt.retained_owner_bytes == receipt.admitted_bytes
        && receipt.admitted_bytes
            == group.persisted_format6_reader->admitted_bytes()
        && receipt.generic_source_fallback_calls == 0u
        && receipt.failures == 0u
        && !receipt.saturated;
}

/// Format-5 counterpart of the concurrent-reader proof. The reader owns only
/// independent fragment address plans. Query routing/count reduction is
/// transient wave state, and every persisted posting remains owned by exactly
/// one fragment.
[[nodiscard]] inline bool shape_v5_parallel_reader_identity_valid(
        const BatchProcessor::CpuPreparedIndexGroup& group,
        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_shape_v5_reader.has_value()
		|| !group.persisted_shape_v5_mapping_guard.has_value()
		|| !*group.persisted_shape_v5_mapping_guard
        || group.persisted_grouped32_reader.has_value()
		|| group.persisted_mps_shape_v5_reader.has_value()
        || group.persisted_format6_reader.has_value()
        || group.target_index
        || group.target_index_source
            != ::bioseq::pipeline_stats::TargetIndexSource::persisted
        || group.persisted_generation_id == 0u
        || group.persisted_source_identity == 0u
        || group.persisted_format6_source_identity_present
        || group.persisted_format6_source_identity
            != std::array<std::byte, 16>{}
        || !group.query_hash_union.empty()
        || group.query_hash_union.capacity() != 0u
        || group.persisted_shape_v5_reader->target_length()
            != target_bp_size) {
        return false;
    }
    const auto& read = group.persisted_read_stats;
    const auto& reader = *group.persisted_shape_v5_reader;
    const auto memory =
        reader.memory_evidence();
    const bool exact_lookup_backing_identity = [&]() noexcept {
        switch (group.persisted_shape_v5_lookup_route) {
        case bioseq::indexing::FragmentAnchorLookupRoute::
                compact_h32_direct:
            return reader.compact_direct_ready()
                && reader.compact_direct_only()
                && reader.mapped_format5_payload_bytes() == 0u
                && reader.format5_directory_entry_count() == 0u
                && memory.directory_capacity_bytes == 0u
                && memory.ordinal_route_capacity_bytes == 0u
                && read.directory_chunks_read == 0u
                && read.directory_bytes_read == 0u;
        case bioseq::indexing::FragmentAnchorLookupRoute::
                format5_control_scans:
            return !reader.compact_direct_ready()
                && !reader.compact_direct_only()
                && reader.mapped_format5_payload_bytes() > 0u
                && reader.format5_directory_entry_count() > 0u
                && memory.directory_capacity_bytes > 0u
                && memory.ordinal_route_capacity_bytes > 0u
                && read.directory_chunks_read > 0u
                && read.directory_bytes_read > 0u;
        }
		return false;
	}();
	const bool deferred_query_set_identity =
		group.compact_direct_query_set_proof_pending
		? group.persisted_shape_v5_lookup_route
				== bioseq::indexing::FragmentAnchorLookupRoute::
					compact_h32_direct
			&& group.canonical_query_hash_union_u32.empty()
			&& group.canonical_query_hash_union_u32.capacity() == 0u
			&& group.key_set_checksum == 0u
			&& !group.build_set_checksum_present
			&& group.build_set_checksum == 0u
		: group.build_set_checksum_present
			&& group.build_set_checksum == group.key_set_checksum;
	return reader.ready()
		&& exact_lookup_backing_identity
		&& deferred_query_set_identity
        && memory.query_record_count == 0u
        && memory.query_record_capacity_bytes == 0u
        && memory.query_hash_capacity_bytes == 0u
        && memory.capacity_accounting_passes == 1u
        && memory.global_position_storage_preparations == 0u
        && memory.global_position_storage_allocations == 0u
        && memory.position_append_growth_reservations == 0u
        && memory.retained_position_count == 0u
        && memory.retained_position_capacity == 0u
        && memory.global_position_capacity_bytes == 0u
        && memory.query_record_position_capacity_bytes == 0u
        && memory.retained_selected_control_capacity_bytes == 0u
        && memory.current_selected_control_capacity_bytes == 0u
        && memory.selected_extent_capacity_bytes == 0u
        && memory.control_payload_capacity_bytes == 0u
        && memory.retained_layout_equation_holds()
        && memory.owned_capacity_bytes
            == reader.admitted_bytes()
        && read.relation_opens == 1u
        && read.head_scans == 1u
        && read.manifest_directory_scans == 1u
        && read.index_scans == 2u
        && read.metadata_repeated_reads == 0u
        && read.full_generation.scan_calls == 0u
        && read.bulk_read_calls == 0u
        && read.range_scan_calls == 0u
        && read.payload_heap_rows_read == 0u
        && read.payload_bytes_read == 0u
        && read.source_parent_rows_read == 1u
        && read.source_catalog_rows_read == 1u
        && read.source_catalog_index_scans == 1u
        && read.source_fragment_rows_read == 0u
        && read.source_decoded_bp == 0u;
}

/// Attach the retained query-sketch proof after a worker has joined. The
/// prebuilt-minimizer entry point must not publish its own query proof; this
/// check makes duplicate attachment fail closed on the PostgreSQL thread.
inline void attach_retained_format6_query_proof(
        MatchResultCollection& collection,
        const pipe::MinimizerRangeExecutionProof& proof) {
    const bool exact_query_proof_absent =
        !collection.exact_simd_query_execution_proof_present
        && collection.exact_simd_query_execution_proof
            == decltype(collection.exact_simd_query_execution_proof){};
    const bool scalar_query_proof_absent =
        collection.scalar_pipeline_execution_proof.query_minimizers
            == decltype(collection.scalar_pipeline_execution_proof
                .query_minimizers){};
    const bool paper_query_proof_absent =
        !collection.paper_nthash_query_execution_proof_present
        && collection.paper_nthash_query_execution_proof
            == decltype(collection.paper_nthash_query_execution_proof){};
    if (!scalar_query_proof_absent
        || !exact_query_proof_absent
        || !paper_query_proof_absent) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "persisted query sketch proof was recorded more than once");
    }
    if (proof.exact_simd_present) {
        collection.record_exact_simd_query_execution_proof(
            proof.exact_simd);
    }
    if (proof.paper_nthash_present) {
        collection.record_paper_nthash_query_execution_proof(
            proof.paper_nthash);
    }
    if (proof.scalar_present) {
        collection.record_scalar_query_minimizer_execution_proof(
            proof.scalar);
    }
}
#endif

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
	std::uint64_t query_sketch_host_wall_ns = 0;
	auto* cpu_prepared_target = scanner_pg_ctx->cpu_prepared_target;
	if (cpu_prepared_target != nullptr) {
		if (selected_vulkan_execution(params)) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_FEATURE_NOT_SUPPORTED,
				"Vulkan execution cannot consume a CPU prepared target");
		}
		if (!cpu_prepared_target->unions_ready
			|| cpu_prepared_target->target_rowno
				!= scanner_pg_ctx->target_rownos[i]
			|| cpu_prepared_target->target_bp_size
				!= static_cast<std::uint64_t>(target_bp_size)) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_DATA_CORRUPTED,
				"BioseqBatchJoin CPU prepared-target identity is invalid");
		}
	}
    scanner_pg_ctx->target_index_sources[i] =
        ::bioseq::pipeline_stats::TargetIndexSource::recomputed;
    std::optional<std::vector<MinimapSketchData>> row_query_minimizers;
    std::optional<pipe::MinimizerRangeExecutionProof>
        row_query_minimizer_proof;
    const BatchProcessor::CpuPreparedQuerySketch*
        retained_prepared_query_sketch = nullptr;
    const auto unobserved_query_view =
        bioseq::storage::as_lazy_sequence_view(query_seq);
    const auto lazy_query_view =
        unobserved_query_view.with_payload_allocation_observer(
            std::addressof(
                scanner_pg_ctx->mmsearch_query_payload_observer));
    const auto fq_pg_view = lazy_query_view;
    const auto& loc_opt = scanner_pg_ctx->target_locators[i];
    const bool persisted_required =
        scanner_pg_ctx->index_read_requirement
        == bioseq::indexing::IndexReadRequirement::
            require_compatible_persisted;
    if (persisted_required
		&& (cpu_prepared_target == nullptr
			|| selected_vulkan_execution(params))) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[i],
			"the live grouped32 reader is available only through the retained "
			"CPU prepared-target lifecycle; GPU and ad-hoc row fallback are forbidden");
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
        using TargetBinding =
            bioseq::storage::postgres_lazy_sequence_binding<S>;
        const TargetBinding target_binding{
            scanner_pg_ctx->db_oid,
            scanner_pg_ctx->tablespace_oid,
            scanner_pg_ctx->main_relfilenode,
            scanner_pg_ctx->mmsearch_seq_attno,
            *loc_opt,
            _padding,
            scanner_pg_ctx->snapshot,
            scanner_pg_ctx->caches,
            scanner_pg_ctx->parent_mctx};
        bioseq::storage::with_lazy_postgres<S>(
            target_binding,
            [scanner_pg_ctx, cpu_prepared_target, &dispatch_params, &params,
             min_bp, per_row_query_mode, persisted_required,
             &row_query_minimizers,
             &cached_query_minimizers, &row_query_minimizer_proof,
             &cached_query_minimizer_proof, &query_seq, lazy_query_view,
             fq_pg_view, target_bp_size, alphabet_code,
             storage_unit_byte_width, i, &loc_opt, _padding, &pool,
             &collection, &row_consumed_persisted_query_sketch,
             &retained_prepared_query_sketch,
             &query_sketch_host_wall_ns](auto ft_pg_view) {
        ft_pg_view = ft_pg_view.with_payload_allocation_observer(
            std::addressof(
                scanner_pg_ctx->mmsearch_target_payload_observer));
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
        if ((persisted_required
			|| (scanner_pg_ctx->read_index_active
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
                 &query_seq, lazy_query_view, &idx_params,
                 &row_consumed_persisted_query_sketch,
                 &query_sketch_host_wall_ns] {
                if (!query_minimizers.has_value()) {
                    if (!minimizer_query_length_is_representable<
                            MinimapSketchData>(
                                static_cast<sequence_size_t>(
                                    query_seq.bp_size()))) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                            "query exceeds the 28-bit minimizer position domain");
                    }
                    const std::uint64_t query_sketch_started_ns =
                        dispatch_host_now_ns();
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
                        lazy_query_view,
                        0,
                        static_cast<sequence_size_t>(query_seq.bp_size()),
                        static_cast<size_t>(idx_params.window_size),
                        static_cast<size_t>(idx_params.kmer_size),
                        backend,
                        pipe::ExactSimdRangeRoute::require_continuation);
					if (!pipe::minimizer_range_proof_matches_backend(
							backend,
							proof,
							/*require_nonempty_vector_work=*/false,
							pipe::ExactSimdRangeRoute::require_continuation)) {
						raise_typed_dispatch_pg_error_as_cpp(
							ERRCODE_INTERNAL_ERROR,
							"persisted query sketch proof does not match the selected "
							"minimizer backend");
					}
                    query_minimizers.emplace(std::move(qc).extract_sketches_data());
                    query_minimizer_proof.emplace(std::move(proof));
					query_sketch_host_wall_ns = elapsed_dispatch_host_ns(
						query_sketch_started_ns);
				} else if (!query_minimizer_proof.has_value()) {
					raise_typed_dispatch_pg_error_as_cpp(
						ERRCODE_DATA_CORRUPTED,
						"cached persisted query minimizers have no operation proof");
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
				raise_typed_dispatch_pg_error_as_cpp(
					ERRCODE_DATA_CORRUPTED,
					"BioseqBatchJoin prepared query group is missing");
			}
			if (persisted_required) {
				const bool has_grouped32_reader = prepared_group != nullptr
					&& prepared_group->persisted_grouped32_reader.has_value();
				const bool has_shape_v5_reader = prepared_group != nullptr
					&& prepared_group->persisted_shape_v5_reader.has_value();
				const bool has_shape_v5_mapping_guard =
					prepared_group != nullptr
					&& prepared_group->persisted_shape_v5_mapping_guard.has_value()
					&& static_cast<bool>(
						*prepared_group->persisted_shape_v5_mapping_guard);
				const bool has_mps_shape_v5_reader = prepared_group != nullptr
					&& prepared_group->
						persisted_mps_shape_v5_reader.has_value();
				const bool has_format6_reader = prepared_group != nullptr
					&& prepared_group->persisted_format6_reader.has_value();
				const bool has_full_format6_source_identity =
					prepared_group != nullptr
					&& prepared_group->
						persisted_format6_source_identity_present;
				const bool format6_identity_valid =
					prepared_group == nullptr
					|| (!has_format6_reader
						? !has_full_format6_source_identity
							&& prepared_group->
								persisted_format6_source_identity
									== std::array<std::byte, 16>{}
						: has_full_format6_source_identity
							&& prepared_group->persisted_source_identity
								== format6_source_diagnostic_token(
									prepared_group->
										persisted_format6_source_identity));
				const bool canonical_owner_exclusive =
					prepared_group == nullptr
					|| (has_format6_reader || has_shape_v5_reader
						|| has_mps_shape_v5_reader
						? prepared_group->query_hash_union.capacity() == 0u
						: prepared_group->
							canonical_query_hash_union_u32.capacity() == 0u);
				bool format6_read_receipt_valid = !has_format6_reader;
				if (has_format6_reader) {
					const auto& receipt =
						prepared_group->persisted_read_stats.format6;
					const auto& memory =
						prepared_group->persisted_format6_reader->
							memory_evidence();
					const std::uint64_t phase_peak = std::max({
						receipt.directory_phase_peak_bytes,
						receipt.control_phase_peak_bytes,
						receipt.posting_phase_peak_bytes,
					});
					format6_read_receipt_valid =
						receipt.format_version
							== bioseq::indexing::kMinimizerFormat6Version
						&& receipt.sessions == 1u
						&& receipt.generation_id
							== prepared_group->persisted_generation_id
						&& receipt.source_identity
							== prepared_group->
								persisted_format6_source_identity
						&& receipt.directory_row_crc_receipts
							== receipt.directory_rows
						&& receipt.control_row_crc_receipts
							== receipt.control_rows
						&& receipt.posting_row_crc_receipts
							== receipt.posting_rows
						&& receipt.control_block_crc_receipts
							== receipt.selected_block_count
						&& receipt.control_semantic_receipts
							== receipt.selected_block_count
						&& receipt.posting_ranges_delivered
							== receipt.posting_semantic_receipts
						&& receipt.posting_range_crc_receipts
							<= receipt.posting_ranges_delivered
						&& receipt.query_count == memory.query_count
						&& receipt.accepted_hash_count
							== memory.accepted_hash_count
						&& receipt.retained_position_count
							== memory.retained_position_count
						&& receipt.peak_admitted_bytes == phase_peak
						&& receipt.retained_owner_bytes
							== receipt.admitted_bytes
						&& receipt.admitted_bytes
							== prepared_group->
								persisted_format6_reader->
									admitted_bytes()
						&& receipt.generic_source_fallback_calls == 0u
						&& receipt.failures == 0u
						&& !receipt.saturated;
				}
				if (prepared_group == nullptr
					|| !BatchProcessor::exactly_one_native_persisted_reader(
							has_grouped32_reader,
							has_shape_v5_reader,
							has_mps_shape_v5_reader,
							has_format6_reader)
					|| has_shape_v5_reader != has_shape_v5_mapping_guard
					|| (has_grouped32_reader
						&& !prepared_group->persisted_grouped32_reader->ready())
					|| (has_shape_v5_reader
						&& !prepared_group->persisted_shape_v5_reader->ready())
					|| (has_mps_shape_v5_reader
						&& !prepared_group->
							persisted_mps_shape_v5_reader->ready())
					|| prepared_group->target_index
					|| prepared_group->target_index_source
						!= ::bioseq::pipeline_stats::TargetIndexSource::persisted
					|| prepared_group->persisted_generation_id == 0
					|| prepared_group->persisted_source_identity == 0
					|| !format6_identity_valid
					|| !canonical_owner_exclusive
					|| !format6_read_receipt_valid) {
					raise_required_persisted_index_error(
						scanner_pg_ctx,
						scanner_pg_ctx->target_rownos[i],
						"the exact READY native persisted reader is absent, incomplete, duplicated, or "
						"mixed with a legacy assembled index");
					}
						if (has_format6_reader || has_shape_v5_reader
							|| has_mps_shape_v5_reader) {
							if (scanner_pg_ctx->explicit_query_ordinals) {
								retained_prepared_query_sketch =
									prepared_group->find_retained_query_sketch(
										scanner_pg_ctx->query_ordinals[i]);
							}
							if (retained_prepared_query_sketch != nullptr) {
								const auto backend =
									pipe::target_index_sketch_backend_from_params(
										idx_params);
								if (retained_prepared_query_sketch->query_bp_size
										!= static_cast<std::uint64_t>(
											query_seq.bp_size())
									|| !pipe::minimizer_range_proof_matches_backend(
										backend,
										retained_prepared_query_sketch->proof,
										/*require_nonempty_vector_work=*/false,
										pipe::ExactSimdRangeRoute::
											require_continuation)) {
									raise_typed_dispatch_pg_error_as_cpp(
										ERRCODE_DATA_CORRUPTED,
										"retained native query sketch proof does not "
										"match the selected minimizer backend");
								}
								row_consumed_persisted_query_sketch = true;
							} else {
								ensure_q();
							}
							const std::vector<MinimapSketchData>&
								alignment_query_minimizers =
									retained_prepared_query_sketch != nullptr
									? retained_prepared_query_sketch->minimizers
									: *query_minimizers;
							if (has_format6_reader) {
							collection = minimap2_port_with_index_pg_checked(
								fq_pg_view,
								ft_pg_view,
							alignment_query_minimizers,
							*prepared_group->persisted_format6_reader,
								idx_params,
								/*gpu_index_cache=*/nullptr,
								bioseq::indexing::GpuIndexCacheIdentity{});
							} else if (has_shape_v5_reader) {
							collection = run_aligner_pg_checked(
								"fragment-native single-query anchor preparation",
									[&alignment_query_minimizers, &query_seq,
									 &idx_params, prepared_group, fq_pg_view,
									 ft_pg_view]() -> MatchResultCollection {
									const std::uint64_t prepare_started_ns =
										dispatch_host_now_ns();
									auto& admission = bioseq::parallelism::
										MemoryAdmissionController::instance();
									auto scratch_result = bioseq::indexing::
										QueryWaveScratch::create(admission);
									if (!scratch_result) {
										throw bioseq::indexing::
											PersistedMinimizerLookupError(
												"fragment-native query scratch was not "
												"admitted; wire_error="
												+ std::to_string(static_cast<unsigned>(
													scratch_result.error())));
									}
									auto scratch = std::move(*scratch_result);
									const bioseq::indexing::FragmentSeedQuery request{
										.minimizers = alignment_query_minimizers,
										.query_length = static_cast<sequence_size_t>(
											query_seq.bp_size()),
										.kmer_size = idx_params.kmer_size,
									};
									FragmentAnchorWaveCompletion wave_completion{
										scratch};
									auto prepared = prepared_group->
										persisted_shape_v5_reader->
										prepare_fragment_anchor_batch(
											admission,
											std::span<const bioseq::indexing::
												FragmentSeedQuery>{&request, 1u},
											idx_params.max_occ,
											scratch,
											/*baseline_query_plan_matcher_for_testing=*/false,
											/*verify_compositional_order_for_testing=*/false,
											prepared_group->
												persisted_shape_v5_lookup_route);
									const auto query_index = prepared.query_view(0u);
									const std::uint64_t prepare_host_wall_ns =
										elapsed_dispatch_host_ns(prepare_started_ns);
									MatchResultCollection out = minimap2_port_with_index(
										fq_pg_view,
										ft_pg_view,
										alignment_query_minimizers,
										query_index,
										idx_params,
										/*gpu_index_cache=*/nullptr,
										bioseq::indexing::GpuIndexCacheIdentity{});
										out.record_seed_lookup_host_time(
											prepare_host_wall_ns);
										out.record_fragment_native_query_evidence(
											prepared.evidence());
										return out;
								});
							} else {
							collection = minimap2_port_with_index_pg_checked(
								fq_pg_view,
								ft_pg_view,
								alignment_query_minimizers,
								*prepared_group->
									persisted_mps_shape_v5_reader,
								idx_params,
								/*gpu_index_cache=*/nullptr,
								bioseq::indexing::GpuIndexCacheIdentity{});
							}
						} else {
						ensure_q();
							collection = minimap2_port_with_index_pg_checked(
								fq_pg_view,
								ft_pg_view,
								*query_minimizers,
								*prepared_group->persisted_grouped32_reader,
								idx_params,
								/*gpu_index_cache=*/nullptr,
								bioseq::indexing::GpuIndexCacheIdentity{});
					}
				if (prepared_group->persisted_reader_queries == 0u
					&& prepared_group->persisted_reader_load_host_wall_ns != 0u) {
					collection.record_persisted_index_decode_host_time(
						prepared_group->persisted_reader_load_host_wall_ns);
				}
				++prepared_group->persisted_reader_queries;
				assembled = true;
				scanner_pg_ctx->target_index_sources[i] =
					::bioseq::pipeline_stats::TargetIndexSource::persisted;
				scanner_pg_ctx->persisted_generation_ids[i] =
					prepared_group->persisted_generation_id;
				scanner_pg_ctx->persisted_source_identities[i] =
					prepared_group->persisted_source_identity;
			}
			if (scanner_pg_ctx->read_index_active
				&& !persisted_required
				&& prepared_group != nullptr
				&& prepared_group->target_index) {
				ensure_q();
				collection = minimap2_port_with_index_pg_checked(
					fq_pg_view,
					ft_pg_view,
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
                         &make_cache_key, fq_pg_view, ft_pg_view,
		                 pg_gpu_index_cache_ptr, prepared_group,
		                 cpu_prepared_target](
                                IStore& fragment_store,
		                                            std::string_view cache_index_name) {
	                                    ensure_q();
	                                    const sequence_size_t total_bp =
	                                        loc_opt->total_bp_size;
	                                    const std::size_t a_lpu =
	                                        compact_sequence_range<S>::traits::letters_per_unit;
	                                    const std::size_t frag_units =
	                                        static_cast<std::size_t>(
	                                            TargetBinding::fragment_bp_capacity())
	                                        / a_lpu;
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
	                                        [&make_cache_key, &assemble_one,
	                                         fq_pg_view, ft_pg_view,
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
	                                                fq_pg_view,
	                                                ft_pg_view,
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
		                                                raise_typed_dispatch_pg_error_as_cpp(
		                                                    ERRCODE_OUT_OF_MEMORY,
		                                                    "BioseqBatchJoin prepared persisted target "
		                                                    "index cannot be admitted");
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
	                                                fq_pg_view,
	                                                ft_pg_view,
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
	                                                fq_pg_view,
	                                                ft_pg_view,
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
											&& !persisted_required
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
		                                    with_checked_legacy_fragment_reader(
		                                        seq_store, [&] {
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
	                                                [&make_cache_key, &seq_store,
	                                                 fq_pg_view, ft_pg_view,
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
	                                                        fq_pg_view,
	                                                        ft_pg_view,
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
		                                                        raise_typed_dispatch_pg_error_as_cpp(
		                                                            ERRCODE_OUT_OF_MEMORY,
		                                                            "BioseqBatchJoin prepared persisted target "
		                                                            "index cannot be admitted");
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
	                                                        fq_pg_view,
	                                                        ft_pg_view,
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
	                                                        fq_pg_view,
	                                                        ft_pg_view,
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
		                                        });
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
		                                    with_checked_legacy_fragment_reader(
		                                        frag_store, [&] {
		                                    if (frag_store.number_of_chunks(
	                                            bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
	                                        run_fragment_backed_index(
		                                            frag_store,
		                                            kMinimizersPerFragmentIndex);
		                                    }
		                                        });
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
                    fq_pg_view,
                    ft_pg_view,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->total_bp_size),
                    compact_sequence_range<S>::traits::letters_per_unit);
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
	                                                ram_store, ft_pg_view,
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
                    const std::size_t a_lpu =
                        compact_sequence_range<S>::traits::letters_per_unit;
                    const std::size_t frag_units =
                        static_cast<std::size_t>(
                            TargetBinding::fragment_bp_capacity())
                        / a_lpu;
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
                        [&make_cache_key, &assemble_ram_one,
                         fq_pg_view, ft_pg_view,
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
                                    fq_pg_view,
                                    ft_pg_view,
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
                                fq_pg_view,
                                ft_pg_view,
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
                fq_pg_view,
                ft_pg_view,
                params,
                static_cast<sequence_size_t>(
                    loc_opt->total_bp_size),
                compact_sequence_range<S>::traits::letters_per_unit);
			// build_and_align_transient_mps always builds and queries the
			// persisted MPS scheme. Pass that concrete scheme to staging even
			// when the short-query heuristic changed the outer alignment pair;
			// ordinary streaming callers retain their requested k/w and still
			// fail closed in can_defer_recompute_minimizer_persist().
			Minimap2Params mps_stage_params = params;
			mps_stage_params.kmer_size = 15;
			mps_stage_params.window_size = 10;
			const bool async_staged =
				stage_recompute_mps_build_if_selected(
					*scanner_pg_ctx, *loc_opt, rowno,
					alphabet_code, storage_unit_byte_width,
					static_cast<std::uint16_t>(_padding),
					mps_stage_params,
					"real_mps_active_policy_noindex");
			bioseq_log<DBG>("[bioseq] index: recompute real-mps-noindex-policy row={} target_bp={} async_staged={} persisted=false backend={}",
                    static_cast<unsigned long long>(rowno),
                    static_cast<unsigned long long>(
                        loc_opt->total_bp_size),
					async_staged,
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
                    fq_pg_view,
                    ft_pg_view,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->total_bp_size),
                    compact_sequence_range<S>::traits::letters_per_unit);
				const bool async_staged =
					stage_recompute_mps_build_if_selected(
						*scanner_pg_ctx, *loc_opt, rowno,
						alphabet_code, storage_unit_byte_width,
						static_cast<std::uint16_t>(_padding),
						params, "real_mps_fast_noindex");
				bioseq_log<DBG>("[bioseq] index: recompute real-mps-fast-noindex row={} index={} target_bp={} async_staged={} persisted=false backend={}",
                        static_cast<unsigned long long>(
                            rowno),
                        fast_mps_choice->index_name,
                        static_cast<unsigned long long>(
                            loc_opt->total_bp_size),
						async_staged,
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
                        ft_pg_view,
                        storage_unit_byte_width,
                        static_cast<std::uint64_t>(
                            scanner_pg_ctx->target_rownos[i]));
                if (resident.has_value()) {
                    const auto resident_target_view =
                        resident->owner.view();
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
                                lazy_query_view,
                                resident_target_view,
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
                            lazy_query_view, resident_target_view, params);
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
                        fq_pg_view, ft_pg_view, params);
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
                        ? minimap_port_segmented_pg_checked(
                            fq_pg_view, ft_pg_view, params)
                        : minimap_port_pg_checked(
                            fq_pg_view, ft_pg_view, params);
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
        });
    } else {
        compact_sequence_range<S> target_seq{*target_hdr};
        const auto target_view =
            bioseq::storage::as_lazy_sequence_view(target_seq);
        collection =
            (target_bp_size > kPgSegmentationThresholdBp)
                ? minimap_port_segmented_pg_checked(
                    lazy_query_view, target_view, params)
                : minimap_port_pg_checked(
                    lazy_query_view, target_view, params);
    }

    if (query_sketch_host_wall_ns != 0u) {
        collection.record_query_sketch_host_time(
            query_sketch_host_wall_ns);
    }
    if (row_consumed_persisted_query_sketch) {
		const pipe::MinimizerRangeExecutionProof* completed_query_proof =
			retained_prepared_query_sketch != nullptr
				? &retained_prepared_query_sketch->proof
				: per_row_query_mode
					? row_query_minimizer_proof
						? &*row_query_minimizer_proof
						: nullptr
					: cached_query_minimizer_proof
						? &*cached_query_minimizer_proof
						: nullptr;
		if (completed_query_proof == nullptr) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_DATA_CORRUPTED,
				"consumed persisted query minimizers have no operation proof");
        }
        const bool exact_query_proof_absent =
            !collection.exact_simd_query_execution_proof_present
            && collection.exact_simd_query_execution_proof
                == decltype(collection.exact_simd_query_execution_proof){};
		const bool scalar_query_proof_absent =
			collection.scalar_pipeline_execution_proof.query_minimizers
				== decltype(collection.scalar_pipeline_execution_proof
					.query_minimizers){};
		const bool paper_query_proof_absent =
			!collection.paper_nthash_query_execution_proof_present
			&& collection.paper_nthash_query_execution_proof
				== decltype(collection.paper_nthash_query_execution_proof){};
		if (!scalar_query_proof_absent
			|| !exact_query_proof_absent
			|| !paper_query_proof_absent) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_INTERNAL_ERROR,
				"persisted query sketch proof was recorded more than once");
        }
        if (completed_query_proof->exact_simd_present) {
            collection.record_exact_simd_query_execution_proof(
                completed_query_proof->exact_simd);
        }
        if (completed_query_proof->paper_nthash_present) {
            collection.record_paper_nthash_query_execution_proof(
                completed_query_proof->paper_nthash);
        }
        if (completed_query_proof->scalar_present) {
            collection.record_scalar_query_minimizer_execution_proof(
                completed_query_proof->scalar);
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
    compact_sequence_range<S> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::as_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::as_lazy_sequence_view(target_seq);
    return minimap_port_pg_checked(
        lazy_query_view, lazy_target_view, params);
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
    compact_sequence_range<S> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::as_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::as_lazy_sequence_view(target_seq);
    return minimap_port_segmented_pg_checked(
        lazy_query_view, lazy_target_view, params);
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
    MatchResultCollection collection;
    bioseq::storage::with_lazy_test_chunked(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_pg_checked(
                query_view, target_view, params);
        });
    return collection;
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
    MatchResultCollection collection;
    bioseq::storage::with_lazy_test_chunked(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_segmented_pg_checked(
                query_view, target_view, params);
        });
    return collection;
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
    compact_sequence_range<S> target_seq{*target_hdr};
    if (chunked_scanner_forced) {
        MatchResultCollection collection;
        bioseq::storage::with_lazy_test_chunked(
            query_seq, target_seq,
            [&collection, &params, target_bp_size](
                    auto query_view, auto target_view) {
                collection = target_bp_size > kPgSegmentationThresholdBp
                    ? minimap_port_segmented_pg_checked(
                        query_view, target_view, params)
                    : minimap_port_pg_checked(
                        query_view, target_view, params);
            });
        return collection;
    }
    const auto lazy_query_view =
        bioseq::storage::as_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::as_lazy_sequence_view(target_seq);
    return (target_bp_size > kPgSegmentationThresholdBp)
        ? minimap_port_segmented_pg_checked(
            lazy_query_view, lazy_target_view, params)
        : minimap_port_pg_checked(
            lazy_query_view, lazy_target_view, params);
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

	    const std::optional<std::string> force_chunked_guc =
	        pg_guc_value_as_cpp("bioseq.test_force_chunked_scanner");
	    const bool chunked_scanner_forced =
	        force_chunked_guc.has_value() && *force_chunked_guc == "on";
            // DOC0264 §9.2 / DOC0274 — test parity knob. When on, the dispatch
            // loop forces the Phase B baseline (with_lazy_in_memory). Otherwise
            // (default) and given a non-null ScannerPgContext, the loop uses
            // the canonical Phase C PG-backed facade (for stub targets,
            // in-memory for the query literal / inline targets).
	    const std::optional<std::string> force_inmemory_guc =
	        pg_guc_value_as_cpp("bioseq.test_force_inmemory_scanner");
	    const bool inmemory_scanner_forced =
	        force_inmemory_guc.has_value() && *force_inmemory_guc == "on";
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
					const std::string diagnostic = fmt::format(
						"relation={} relation_backed={} source={} index={}",
						static_cast<unsigned>(scanner_pg_ctx->user_relid),
						relation_backed,
						bioseq::storage::indexing::
							active_read_policy_resolution_source_name(source),
						scanner_pg_ctx->active_index);
					raise_typed_dispatch_pg_error_as_cpp(
						ERRCODE_INTERNAL_ERROR,
						"active-read policy dispatch proof is invalid",
						diagnostic.c_str());
				}
			}
            if (index_read_requirement
					== bioseq::indexing::IndexReadRequirement::require_absent
				&& scanner_pg_ctx != nullptr
				&& scanner_pg_ctx->read_index_active) {
				raise_typed_dispatch_pg_error_as_cpp(
					ERRCODE_INTERNAL_ERROR,
					"index_read_requirement=require_absent contract violation",
					"typed processor received read_index_active=true");
            }
			const bool selected_vulkan =
				selected_vulkan_execution(dispatch_params);
			const bool cpu_persisted_prepared = scanner_pg_ctx != nullptr
				&& scanner_pg_ctx->cpu_prepared_target != nullptr
				&& scanner_pg_ctx->gpu_prepared_shape_v5_target == nullptr;
			const bool gpu_persisted_prepared = scanner_pg_ctx != nullptr
				&& scanner_pg_ctx->cpu_prepared_target == nullptr
				&& scanner_pg_ctx->gpu_prepared_shape_v5_target != nullptr
				&& scanner_pg_ctx->gpu_prepared_shape_v5_target->ready();
            if (persisted_required
				&& (selected_vulkan
					? !gpu_persisted_prepared : !cpu_persisted_prepared)) {
                raise_required_persisted_index_error(
                    scanner_pg_ctx,
                    -1,
					selected_vulkan
						? "ordinal 2 GPU execution requires one retained authenticated Format-5/ABI3 target and forbids a CPU prepared target"
						: "ordinal 2 CPU execution requires one retained native persisted reader and forbids a GPU prepared target");
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
			bool persisted_transaction_active = false;

            if (selected_vulkan_execution(dispatch_params)
                && !pipe::complete_all_gpu_tuple_selected(dispatch_params)) {
				add_operational_counter(
					stats, stats.operational.backend_declines);
                bioseq::gpu::vulkan::VulkanRuntimeError err{
                    "requested Vulkan components do not form the complete "
                    "supported all-GPU tuple; generic CPU/SIMD biological "
                    "fallback is forbidden"};
                raise_vulkan_pg_error(
                    "Vulkan complete-tuple admission", err.what());
            }

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
				if (gpu_persisted_prepared
					&& (dispatch_params.do_cigar
						|| !pipe::phase_c_coordinate_only_active(
							dispatch_params))) {
					raise_typed_dispatch_pg_error_as_cpp(
						ERRCODE_FEATURE_NOT_SUPPORTED,
						"persisted GPU Format-5 execution does not yet support CIGAR projection",
						"the direct path can execute query sketching, persisted Shape lookup, "
						"GPU chaining, backtracking, and coordinate projection, but no genuine "
						"GPU target-window reader is wired for alignment extension",
						"Use coordinate-only projection; CPU target hydration or SIMD/CPU "
						"alignment fallback is intentionally forbidden.");
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
					persisted_transaction_active = gpu_persisted_prepared;
                    const std::size_t w =
                        static_cast<std::size_t>(dispatch_params.window_size);
                    const std::size_t k =
                        static_cast<std::size_t>(dispatch_params.kmer_size);
                    // The resident query sketch remains device-only.  A fair
                    // no-index target build is complete and query-independent;
                    // host-materialising query hashes to filter that build
                    // would create a direct-match accelerator, not an index.
                    std::shared_ptr<bioseq::gpu::vulkan::
                        VulkanBatchSketchMemoryRetention>
                        query_sketch_memory_retention;
                    std::vector<bioseq::gpu::vulkan::PhaseCSeedQueryRecordWire>
                        query_records;
                    bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>
                        query_minimizer_device_span{};
                    bioseq::gpu::vulkan::DeviceQuerySketchAuthority
                        query_sketch_authority{};
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
                                            device_only);
                        if (!gpu_batch_sketch) {
                            pipe::throw_vulkan_declined(
                                "Vulkan batched query minimizer sketch");
                        }
                        query_records = std::move(gpu_batch_sketch->query_records);
                        query_sketch_memory_retention =
                            gpu_batch_sketch->memory_retention;
                        query_minimizer_device_span =
                            gpu_batch_sketch->device_span();
                        query_sketch_authority =
                            std::move(gpu_batch_sketch->query_authority);
                    }
                    const std::uint64_t gpu_query_sketch_host_ns =
                        gpu_batch_now_ns() - gpu_query_sketch_begin_ns;

					/* A restart-cold persisted GPU target arrives here as three
					 * authenticated PostgreSQL-owned Format-5 buffers.  Consume that
					 * representation directly: do not materialize the target sequence,
					 * fingerprint it, rebuild a legacy slot table, or read target symbols
					 * on the host. */
					auto* const prepared_gpu_target = scanner_pg_ctx != nullptr
						? scanner_pg_ctx->gpu_prepared_shape_v5_target : nullptr;
					if (prepared_gpu_target != nullptr) {
						if (!gpu_persisted_prepared
							|| !prepared_gpu_target->ready()
							|| prepared_gpu_target->resident == nullptr
							|| query_records.size() != query_sidecars.size()
							|| prepared_gpu_target->target_bp_size
								> static_cast<std::uint64_t>(
									(std::numeric_limits<sequence_size_t>::max)())) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 prepared-target identity is invalid");
						}
						const auto requested_source =
							requested_target_index_source(scanner_pg_ctx);
						if (requested_source
								!= ::bioseq::pipeline_stats::TargetIndexSource::persisted
							|| dispatch_params.max_occ < 0
							|| static_cast<std::uint64_t>(dispatch_params.max_occ)
								> prepared_gpu_target->resident_view.max_occ_cap) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 source or max_occ contract is invalid");
						}

						/* Atomic publication keeps the direct inputs, runtime results
						 * and staged SQL values alive together. Admit that transient overlap
						 * before the first covered N-job reserve. */
						const std::uint64_t publication_staging_bytes =
							persisted_shape_v5_publication_staging_bytes(
								static_cast<std::uint64_t>(query_sidecars.size()));
						if (publication_staging_bytes
								== (std::numeric_limits<std::uint64_t>::max)()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 publication staging admission overflow");
						}
						auto publication_staging_permit =
							bioseq::parallelism::MemoryAdmissionController::instance()
								.try_reserve(publication_staging_bytes);
						if (!publication_staging_permit.has_value()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 publication staging admission was rejected");
						}

						using StagedJob = ::bioseq::pipeline_stats::
							PipelineJobExecutionStats;
						std::vector<bioseq::gpu::vulkan::PhaseCSeedCoordinateJob>
							direct_jobs;
						std::vector<std::size_t> direct_job_rows;
						direct_jobs.reserve(query_sidecars.size());
						direct_job_rows.reserve(query_sidecars.size());
						for (const GpuBatchQuerySidecar& query : query_sidecars) {
							const std::size_t row = query.row_index;
							const std::uint32_t query_index =
								row_to_query_index[row];
							if (query_index
									== std::numeric_limits<std::uint32_t>::max()
								|| query_index >= query_records.size()
								|| scanner_pg_ctx->target_rownos[row]
									!= prepared_gpu_target->target_rowno) {
								throw bioseq::gpu::vulkan::VulkanRuntimeError(
									"persisted GPU Format-5 query/target row identity is inconsistent");
							}
							direct_jobs.push_back(
								bioseq::gpu::vulkan::PhaseCSeedCoordinateJob{
									.slots = nullptr,
									.positions = nullptr,
									.target_len =
										prepared_gpu_target->target_bp_size,
									.slot_mask = 0u,
									.position_count = 0u,
									.bucket_count = 0u,
									.target_position_offset = 0u,
									.query_index = query_index,
									.logical_target_row =
										static_cast<std::uint64_t>(
											scanner_pg_ctx->target_rownos[row]),
									.logical_query_ordinal =
										scanner_pg_ctx->explicit_query_ordinals
											? scanner_pg_ctx->query_ordinals[row]
											: per_row_query_mode
												? static_cast<std::uint64_t>(row)
												: broadcast_query_ordinal,
									.shape_v5 =
										&prepared_gpu_target->resident_view,
								});
							direct_job_rows.push_back(row);
						}
						if (direct_jobs.empty()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 batch has no admissible GPU query job");
						}
						if (query_minimizer_device_span.count()
								> static_cast<std::uint64_t>(
									(std::numeric_limits<std::uint32_t>::max)())) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 query minimizer count exceeds ABI");
						}

						bioseq::gpu::vulkan::PhaseCSeedQueryInputs query_inputs{
							.host_minimizer_bytes = {},
							.device_minimizer_words =
								query_minimizer_device_span.words(),
							.minimizer_count = static_cast<std::uint32_t>(
								query_minimizer_device_span.count()),
							.minimizer_word_kind = static_cast<std::uint32_t>(
								bioseq::gpu::vulkan::minimizer_wire_traits<
									MinimizerT>::word_kind),
							.minimizer_words32_per_record =
								bioseq::gpu::vulkan::minimizer_wire_traits<
									MinimizerT>::words32_per_minimizer,
							.hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<
								MinimizerT>::hash_shift,
							.pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<
								MinimizerT>::pos_bits,
							.window_size = static_cast<std::uint32_t>(
								dispatch_params.window_size),
							.host_query_records = std::span<const
								bioseq::gpu::vulkan::PhaseCSeedQueryRecordWire>{
								query_records.data(), query_records.size()},
							.device_query_records = nullptr,
						};
						auto cp = dispatch_params.to_chaining_params();
						const std::int32_t max_dist_x =
							std::max(cp.max_gap, cp.bandwidth);
						const std::int32_t max_dist_y =
							std::max(cp.max_gap, cp.bandwidth);
						const auto chain_key =
							bioseq::chaining::choose_vulkan_chaining_pipeline(
								runtime.features());
						constexpr auto coordinate_algorithm = bioseq::gpu::vulkan::
							PhaseCSeedCoordinateAlgorithm::vulkan_tiled_dp;
						const std::int32_t max_iter =
							std::min<std::int32_t>(cp.max_chain_iter, 3000);
						constexpr std::uint32_t probe_limit = 16u;
						const auto coordinate_configuration =
							bioseq::gpu::vulkan::
								persisted_shape_v5_coordinate_config_identity(
									static_cast<std::uint32_t>(
										dispatch_params.kmer_size),
									dispatch_params.max_occ,
									coordinate_algorithm,
									chain_key,
									max_dist_x,
									max_dist_y,
									cp.bandwidth,
									max_iter,
									cp.gap_penalty,
									cp.skip_penalty,
									cp.max_chain_skip,
									cp.min_chain_score,
									cp.min_seed_count,
									probe_limit);
						const std::uint64_t query_authority_identity =
							query_sketch_authority.identity();
						const auto* const query_minimizer_buffer_identity =
							query_inputs.device_minimizer_words;
						auto coord_run = runtime.
							run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
								query_inputs,
								std::span<const bioseq::gpu::vulkan::
									PhaseCSeedCoordinateJob>{
									direct_jobs.data(), direct_jobs.size()},
								static_cast<std::uint32_t>(
									dispatch_params.kmer_size),
								dispatch_params.max_occ,
								coordinate_algorithm,
								chain_key,
								max_dist_x,
								max_dist_y,
								cp.bandwidth,
								max_iter,
								cp.gap_penalty,
								cp.skip_penalty,
								cp.max_chain_skip,
								cp.min_chain_score,
								cp.min_seed_count,
								probe_limit,
								std::move(query_sketch_authority));

						const auto& direct = coord_run.shape_v5_direct;
						const bool direct_proof_valid = direct.selected
							&& direct.direct_count_scan_emit_completed
							&& direct_job_rows.size() == direct_jobs.size()
							&& direct.problem_count == direct_jobs.size()
							&& coord_run.stats.problem_count == direct_jobs.size()
							&& coord_run.results.size() == direct_jobs.size()
							&& direct.generation_id
								== prepared_gpu_target->resident_view.generation_id
							&& direct.source_identity
								== prepared_gpu_target->resident_view.source_identity
							&& direct.cold_load_identity
								== prepared_gpu_target->resident_view.cold_load_identity
							&& direct.count_dispatches
								== coord_run.stats.seed_count_dispatches
							&& direct.emit_dispatches
								== coord_run.stats.seed_emit_dispatches
							&& direct.count_status_checks == direct.count_dispatches
							&& direct.emit_status_checks == direct.emit_dispatches
							&& direct.status_or == 0u
							&& direct.cpu_or_simd_fallbacks == 0u
							&& coord_run.stats.coordinate_vote_dispatches == 0u
							&& coord_run.operation_proof.sealed()
							&& coord_run.operation_proof.job_count()
								== direct_jobs.size();
						if (!direct_proof_valid) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 direct-route proof is incomplete or inconsistent");
						}

						::bioseq::pipeline_stats::PipelineExecutionStats
							staged_stats{};
						add_stage_host_time(
							staged_stats,
							::bioseq::pipeline_stats::PipelineStage::query_sketch,
							gpu_query_sketch_host_ns);
						add_operational_counter(
							staged_stats,
							staged_stats.operational.gpu_seed_dispatches,
							coord_run.stats.seed_count_dispatches
								+ coord_run.stats.seed_emit_dispatches);
						add_operational_counter(
							staged_stats,
							staged_stats.operational.gpu_chain_dispatches,
							coord_run.stats.chain_dispatches);
						add_operational_counter(
							staged_stats,
							staged_stats.operational.gpu_chain_backtrack_dispatches,
							coord_run.stats.chain_backtrack_dispatches);
						const std::uint64_t resident_cache_hits =
							static_cast<std::uint64_t>(direct_jobs.size())
							- (prepared_gpu_target->dispatch_waves == 0u ? 1u : 0u);
						add_operational_counter(
							staged_stats,
							staged_stats.operational.gpu_persisted_resident_cache_hits,
							resident_cache_hits);

						std::vector<StagedJob> staged_jobs;
						staged_jobs.reserve(direct_jobs.size());
						decltype(results) staged_results;
						std::array<bool, kDispatchBatchCapacity> staged_rows{};

						for (std::size_t problem_index = 0;
							 problem_index < direct_jobs.size();
							 ++problem_index) {
							const std::size_t row = direct_job_rows[problem_index];
							if (row >= batch_size || row >= kDispatchBatchCapacity
								|| staged_rows[row] || query_ids[row] < 0
								|| direct_jobs[problem_index].logical_target_row
									> ::bioseq::pipeline_stats::sql_bigint_value_max
								|| direct_jobs[problem_index].logical_query_ordinal
									> ::bioseq::pipeline_stats::sql_bigint_value_max
								|| prepared_gpu_target->resident_view.generation_id
									> ::bioseq::pipeline_stats::sql_bigint_value_max
								|| scanner_pg_ctx->target_index_sources[row]
									!= ::bioseq::pipeline_stats::TargetIndexSource::none
								|| scanner_pg_ctx->persisted_generation_ids[row] != 0u
								|| scanner_pg_ctx->persisted_source_identities[row] != 0u) {
								throw bioseq::gpu::vulkan::VulkanRuntimeError(
									"persisted GPU Format-5 staged row publication is invalid");
							}
							staged_rows[row] = true;
							const auto& coord = coord_run.results[problem_index];
							const auto& query_record =
								query_records[direct_jobs[problem_index].query_index];
							auto staged_job = stage_persisted_shape_v5_coordinate_job(
								dispatch_params,
								requested_source,
								coord_run.operation_proof,
								bioseq::gpu::vulkan::
									PersistedShapeV5CoordinateJobExpectation{
										.problem_index = problem_index,
										.query_authority_identity =
											query_authority_identity,
										.resident_descriptors = bioseq::gpu::vulkan::
											runtime_buffer_identity(prepared_gpu_target
												->resident_view.descriptors),
										.resident_controls = bioseq::gpu::vulkan::
											runtime_buffer_identity(prepared_gpu_target
												->resident_view.controls),
										.resident_positions = bioseq::gpu::vulkan::
											runtime_buffer_identity(prepared_gpu_target
												->resident_view.positions),
										.query_minimizer_buffer = bioseq::gpu::vulkan::
											runtime_buffer_identity(
												query_minimizer_buffer_identity),
										.device_uuid = prepared_gpu_target
											->resident_view.device_uuid,
										.descriptor_bytes = prepared_gpu_target
											->resident_view.descriptor_bytes,
										.control_bytes = prepared_gpu_target
											->resident_view.control_bytes,
										.position_bytes = prepared_gpu_target
											->resident_view.position_bytes,
										.position_count = prepared_gpu_target
											->resident_view.position_count,
										.max_occ_cap = prepared_gpu_target
											->resident_view.max_occ_cap,
										.partition_count = prepared_gpu_target
											->resident_view.partition_count,
										.abi_version = prepared_gpu_target
											->resident_view.abi_version,
										.generation_id = prepared_gpu_target
											->resident_view.generation_id,
										.source_identity = prepared_gpu_target
											->resident_view.source_identity,
										.cold_load_identity = prepared_gpu_target
											->resident_view.cold_load_identity,
										.target_len = prepared_gpu_target
											->target_bp_size,
										.query_index = direct_jobs[problem_index]
											.query_index,
										.logical_target_row = direct_jobs[problem_index]
											.logical_target_row,
											.logical_query_ordinal = direct_jobs[problem_index]
												.logical_query_ordinal,
											.query_record = query_record,
											.result = coord,
										.configuration =
											coordinate_configuration,
									});
							if (!staged_job.has_value()) {
								continue;
							}
							staged_jobs.push_back(std::move(*staged_job));
							if (coord.valid != 0u) {
								const std::uint32_t query_index =
									direct_jobs[problem_index].query_index;
								staged_results[query_ids[row]].push_back(
									pipe::phase_c_coordinate_to_match_result(
										coord,
										query_sidecars[query_index].query_len,
										static_cast<sequence_size_t>(
											prepared_gpu_target->target_bp_size)));
							}
						}
						const std::uint64_t expected_jobs =
							static_cast<std::uint64_t>(direct_jobs.size());
						const std::uint64_t proof_jobs = static_cast<std::uint64_t>(
							coord_run.operation_proof.job_count());
						const std::uint64_t consumed_jobs =
							static_cast<std::uint64_t>(
								coord_run.operation_proof.consumed_count());
						const std::uint64_t pending_jobs = proof_jobs >= consumed_jobs
							? proof_jobs - consumed_jobs
							: (std::numeric_limits<std::uint64_t>::max)();
						const ::bioseq::pipeline_stats::
							PersistedShapeV5CoordinatePublicationCounts
							publication_counts{
								.expected_jobs = expected_jobs,
								.proof_jobs = proof_jobs,
								.consumed_jobs = consumed_jobs,
								.accepted_jobs = static_cast<std::uint64_t>(
									coord_run.operation_proof.accepted_count()),
								.burned_jobs = static_cast<std::uint64_t>(
									coord_run.operation_proof.burned_count()),
								.pending_jobs = pending_jobs,
								.staged_jobs = static_cast<std::uint64_t>(
									staged_jobs.size()),
							};
						if (!::bioseq::pipeline_stats::
								persisted_shape_v5_coordinate_publication_counts_valid(
									publication_counts)
							|| !coord_run.operation_proof.all_jobs_accepted()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 operation proof was not accepted exactly once");
						}
						const bool lifecycle_prestate_valid =
							prepared_gpu_target->query_count != 0u
							&& prepared_gpu_target->query_count
								<= ::bioseq::pipeline_stats::sql_bigint_value_max
							&& prepared_gpu_target->dispatched_query_count
								<= prepared_gpu_target->query_count
							&& (prepared_gpu_target->dispatched_query_count == 0u
								? prepared_gpu_target->dispatch_waves == 0u
								: prepared_gpu_target->dispatch_waves != 0u)
							&& expected_jobs <= prepared_gpu_target->query_count
								- prepared_gpu_target->dispatched_query_count;
						if (!lifecycle_prestate_valid
							|| prepared_gpu_target->dispatched_query_count
								> ::bioseq::pipeline_stats::sql_bigint_value_max
									- expected_jobs
							|| prepared_gpu_target->dispatch_waves
								>= ::bioseq::pipeline_stats::sql_bigint_value_max) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 lifecycle counters overflow");
						}

						if (!results.empty()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 result staging did not start empty");
						}
						const std::size_t effective_workers =
							bioseq::parallelism::GlobalPool::instance().max_concurrency();
						if (effective_workers == 0u
							|| effective_workers
								> ::bioseq::pipeline_stats::sql_integer_value_max) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 CPU worker identity is invalid");
						}
						const auto cache_metrics_preview =
							coord_run.operation_proof.cache_metrics_preview();
						const auto environment_preview =
							coord_run.operation_proof.environment_preview();
						const auto staged_environment = ::bioseq::pipeline_stats::
							make_persisted_shape_v5_gpu_environment_proof(
								environment_preview, effective_workers);
						const auto maximum_cache_metrics = ::bioseq::pipeline_stats::
							make_persisted_shape_v5_gpu_cache_metrics_delta(
								cache_metrics_preview);
						/* These are immutable/bounded maxima, not counters to publish. They
						 * let every environment, SQL range and prospective-merge rejection
						 * happen before the finalizer claims report-once cache bits. */
						if (!staged_environment.has_value()
							|| !maximum_cache_metrics.has_value()
							|| environment_preview.gpu_device_uuid
								!= prepared_gpu_target->resident_view.device_uuid
							|| (cache_metrics_preview.gpu_pipeline_cache_loads != 0u
								&& cache_metrics_preview.gpu_pipeline_cache_seed_bytes
									!= environment_preview.seed_size)) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 environment/cache preview is invalid");
						}
						for (StagedJob& job : staged_jobs) {
							job.environment = *staged_environment;
							staged_stats.merge_job(job);
						}
						if (record_target_operational_outcomes) {
							record_target_row(staged_stats);
							record_target_index_outcome(
								staged_stats,
								requested_source,
								::bioseq::pipeline_stats::TargetIndexSource::persisted);
						}
						auto maximum_staged_stats = staged_stats;
						maximum_staged_stats.merge_operational(*maximum_cache_metrics);
						auto maximum_prospective_stats = stats;
						maximum_prospective_stats.merge(maximum_staged_stats);
						if (!::bioseq::pipeline_stats::
								persisted_shape_v5_coordinate_prospective_stats_valid(
									stats,
									maximum_staged_stats,
									maximum_prospective_stats,
									expected_jobs)) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 prospective stats are invalid");
						}

						/* SQL evidence is a separate admitted statement-root ledger. Build,
						 * validate and preflight its complete operation image now: nothing may
						 * allocate, search for duplicates or decide capacity after Runtime's
						 * final immutable gate claims report-once cache authority. */
						const auto operation_evidence_preview =
							coord_run.operation_proof.operation_evidence_preview();
						auto operation_evidence_chunk = ::bioseq::pipeline_stats::
							PersistedGpuOperationEvidenceChunk::prepare(
								direct_jobs.size());
						if (!operation_evidence_preview.accepted()
							|| operation_evidence_chunk == nullptr
							|| scanner_pg_ctx->persisted_gpu_operation_evidence_sink
								== nullptr
							|| operation_evidence_preview.device_uuid
								!= prepared_gpu_target->resident_view.device_uuid
							|| operation_evidence_preview.acceptance.operation_identity
								!= cache_metrics_preview.token_identity) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 SQL operation evidence is unavailable");
						}
						operation_evidence_chunk->operation() =
							persisted_gpu_operation_evidence(
								operation_evidence_preview,
								publication_counts,
								prepared_gpu_target->resident_view.generation_id,
								prepared_gpu_target->resident_view.source_identity,
								prepared_gpu_target->resident_view.cold_load_identity);
						for (std::size_t problem_index = 0u;
							 problem_index < direct_jobs.size(); ++problem_index) {
							const auto job_evidence_preview = coord_run.operation_proof.
								job_evidence_preview(problem_index);
							if (!job_evidence_preview.has_value()
								|| job_evidence_preview->audit.operation_identity
									!= operation_evidence_preview.acceptance
										.operation_identity
								|| job_evidence_preview->audit.generation_id
									!= prepared_gpu_target->resident_view.generation_id
								|| job_evidence_preview->audit.source_identity
									!= prepared_gpu_target->resident_view.source_identity
								|| job_evidence_preview->audit.cold_load_identity
									!= prepared_gpu_target->resident_view.cold_load_identity
								|| job_evidence_preview->audit.logical_target_row
									!= direct_jobs[problem_index].logical_target_row
								|| job_evidence_preview->audit.logical_query_ordinal
									!= direct_jobs[problem_index].logical_query_ordinal
								|| job_evidence_preview->audit.query_index
									!= direct_jobs[problem_index].query_index) {
								throw bioseq::gpu::vulkan::VulkanRuntimeError(
									"persisted GPU Format-5 SQL job evidence is inconsistent");
							}
							operation_evidence_chunk->jobs()[problem_index] =
								persisted_gpu_job_evidence(*job_evidence_preview);
						}
						if (!operation_evidence_chunk->seal()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 SQL operation evidence did not seal");
						}
						auto operation_evidence_append =
							scanner_pg_ctx->persisted_gpu_operation_evidence_sink
								->prepare_append(operation_evidence_chunk);
						if (!operation_evidence_append.accepted()) {
							throw bioseq::gpu::vulkan::VulkanRuntimeError(
								"persisted GPU Format-5 SQL evidence ledger rejected the operation");
						}

						/* Every allocation, row check and overflow check is complete.
						 * The runtime finalizer validates the causal receipt union and
						 * performs the final immutable-policy live gate before minting
						 * this move-only authority. */
						auto accepted_operation = runtime.
							finalize_persisted_shape_v5_coordinate_operation(
								std::move(coord_run.operation_proof),
								operation_evidence_preview);
						const auto& acceptance = accepted_operation.audit();
						const auto& authoritative_environment =
							accepted_operation.environment_binding();
						const auto authoritative_cache_metrics =
							accepted_operation.cache_metrics_audit();
						::bioseq::pipeline_stats::PipelineOperationalStats
							authoritative_cache_delta{};
						authoritative_cache_delta.gpu_pipeline_cache_loads =
							authoritative_cache_metrics.gpu_pipeline_cache_loads;
						authoritative_cache_delta.gpu_pipeline_cache_seed_bytes =
							authoritative_cache_metrics.gpu_pipeline_cache_seed_bytes;
						authoritative_cache_delta.gpu_pipeline_cache_load_ns =
							authoritative_cache_metrics.gpu_pipeline_cache_load_ns;
						authoritative_cache_delta.gpu_pipeline_creates =
							authoritative_cache_metrics.gpu_pipeline_creates;
						authoritative_cache_delta.gpu_pipeline_creation_ns =
							authoritative_cache_metrics.gpu_pipeline_creation_ns;
						auto committed_staged_stats = staged_stats;
						committed_staged_stats.merge_operational(
							authoritative_cache_delta);
						auto prospective_stats = stats;
						prospective_stats.merge(committed_staged_stats);

						/* The report-once token must be the final authority staged into the
						 * prospective aggregate. Consume it only when publication is now
						 * inevitable; a copied environment receipt cannot replay these
						 * counters. */
						auto cache_metrics_delta =
							accepted_operation.take_cache_metrics_delta();
						static_assert(std::is_nothrow_copy_constructible_v<
							::bioseq::pipeline_stats::PipelineExecutionStats>);
						static_assert(std::is_nothrow_copy_assignable_v<
							::bioseq::pipeline_stats::PipelineExecutionStats>);
						static_assert(noexcept(
							committed_staged_stats.merge_operational(
								authoritative_cache_delta)));
						static_assert(noexcept(
							prospective_stats.merge(committed_staged_stats)));
						static_assert(std::is_nothrow_move_constructible_v<
							decltype(results)>);
						static_assert(noexcept(results.swap(staged_results)));
						static_assert(noexcept(::bioseq::pipeline_stats::
							persisted_shape_v5_gpu_environment_binding_matches(
								authoritative_environment, environment_preview)));
						static_assert(noexcept(::bioseq::pipeline_stats::
							persisted_shape_v5_gpu_cache_metrics_bounded_by(
								authoritative_cache_metrics, cache_metrics_preview)));
						static_assert(noexcept(cache_metrics_delta.consume()));
						static_assert(noexcept(
							operation_evidence_chunk->publish_cache_metrics(
								persisted_gpu_cache_metrics_evidence(
									authoritative_cache_metrics))));
						static_assert(noexcept(
							scanner_pg_ctx->persisted_gpu_operation_evidence_sink
								->commit_append(
									std::move(operation_evidence_append),
									std::move(operation_evidence_chunk))));
						/* The finalizer compared this same preview before its live gate
						 * and report-once claim. These are construction assertions, not a
						 * recoverable post-finalizer rejection path. */
						DEBUG_ASSERT(accepted_operation.accepted(),
							"persisted GPU accepted operation authority vanished");
						DEBUG_ASSERT(acceptance == operation_evidence_preview.acceptance,
							"persisted GPU operation changed after finalizer");
						DEBUG_ASSERT(acceptance.expected_job_count == expected_jobs,
							"persisted GPU job count changed after finalizer");
						DEBUG_ASSERT(::bioseq::pipeline_stats::
							persisted_shape_v5_gpu_environment_binding_matches(
								authoritative_environment, environment_preview),
							"persisted GPU environment changed after finalizer");
						DEBUG_ASSERT(::bioseq::pipeline_stats::
							persisted_shape_v5_gpu_cache_metrics_bounded_by(
								authoritative_cache_metrics, cache_metrics_preview),
							"persisted GPU cache metrics exceeded preview");
						DEBUG_ASSERT(cache_metrics_delta.accepted()
							&& cache_metrics_delta.audit()
								== authoritative_cache_metrics,
							"persisted GPU cache token changed after finalizer");
						const auto commit_publication = [
							&authoritative_cache_metrics,
							&cache_metrics_delta,
							&results,
							&staged_results,
							&stats,
							&prospective_stats,
							&direct_job_rows,
							&scanner_pg_ctx,
							&prepared_gpu_target,
							&expected_jobs,
							&operation_evidence_chunk,
							&operation_evidence_append]() noexcept {
							operation_evidence_chunk->publish_cache_metrics(
								persisted_gpu_cache_metrics_evidence(
									authoritative_cache_metrics));
							/* accepted() plus the exact audit equality above are consume()'s
							 * complete precondition. Burn the one-shot authority now; no
							 * rejecting edge may follow before SQL-visible publication. */
							cache_metrics_delta.consume();
							results.swap(staged_results);
							stats = prospective_stats;
							for (const std::size_t row : direct_job_rows) {
								scanner_pg_ctx->target_index_sources[row] =
									::bioseq::pipeline_stats::TargetIndexSource::persisted;
								scanner_pg_ctx->persisted_generation_ids[row] =
									prepared_gpu_target->resident_view.generation_id;
								scanner_pg_ctx->persisted_source_identities[row] =
									prepared_gpu_target->resident_view.source_identity;
							}
							prepared_gpu_target->dispatched_query_count += expected_jobs;
							++prepared_gpu_target->dispatch_waves;
							scanner_pg_ctx->persisted_gpu_operation_evidence_sink
								->commit_append(
									std::move(operation_evidence_append),
									std::move(operation_evidence_chunk));
						};
						static_assert(noexcept(commit_publication()));
						commit_publication();
						return results;
					}
					add_stage_host_time(
						stats,
						::bioseq::pipeline_stats::PipelineStage::query_sketch,
						gpu_query_sketch_host_ns);

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
                        const auto target_view =
                            bioseq::storage::as_lazy_sequence_view(target_seq);
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
                            const auto [segment_lo_bp, segment_hi_bp] =
                                pipe::validated_segment_bp_bounds(
                                    range, lpu, target.target_len);
                            const std::uint64_t gpu_index_build_begin_ns =
                                gpu_batch_now_ns();
                            const bool full_target_range =
                                segment_lo_bp == 0u
                                && segment_hi_bp == target.target_len;
                            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
                            auto built = full_target_range
                                ? bioseq::gpu::vulkan::build_gpu_index_on_device<
                                    MinimizerT>(
                                    target_view,
                                    w,
                                    k,
                                    frozen_query_gate,
                                    &decline)
                                : bioseq::gpu::vulkan::build_gpu_segment_index_on_device<
                                    MinimizerT>(
                                    target_view,
                                    segment_lo_bp,
                                    segment_hi_bp,
                                    w,
                                    k,
                                    frozen_query_gate,
                                    &decline);
                            if (!built) {
                                pipe::throw_vulkan_sketch_declined(
                                    "Vulkan batched segment index build",
                                    decline);
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
                            resident->target_position_offset = segment_lo_bp;
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
                    if (query_minimizer_device_span.count()
                            > static_cast<std::uint64_t>(
                                (std::numeric_limits<std::uint32_t>::max)())) {
                        throw bioseq::gpu::vulkan::VulkanRuntimeError{
                            "batched device query minimizer count exceeds ABI"};
                    }
                    bioseq::gpu::vulkan::PhaseCSeedQueryInputs query_inputs{
                        .host_minimizer_bytes = {},
                        .device_minimizer_words =
                            query_minimizer_device_span.words(),
                        .minimizer_count = static_cast<std::uint32_t>(
                            query_minimizer_device_span.count()),
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
					/* The persisted Shape-V5 lane is transactional.  Its proof,
					 * results, stats, source identities and lifecycle remain local on
					 * every failure; do not leak a diagnostic mutation into the caller
					 * while converting the exception to PostgreSQL. */
					if (!persisted_transaction_active) {
						add_operational_counter(
							stats, stats.operational.backend_declines);
					}
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }
            }

			/* A biological Vulkan request may never continue into the generic
			 * in-memory/chunked/scanner row dispatcher. That dispatcher is allowed to
			 * compose CPU stages and is therefore not a fallback for a selected GPU
			 * statement. The dedicated branch above returns on every successful run;
			 * reaching this guard proves that its admission was bypassed (most notably
			 * by the integration-only forced-chunked route). */
			if (selected_vulkan_execution(dispatch_params)) {
				if (!persisted_transaction_active) {
					add_operational_counter(
						stats, stats.operational.backend_declines);
				}
				const std::string detail = chunked_scanner_forced
					? "selected Vulkan execution cannot use the forced chunked scanner; generic CPU/SIMD biological fallback is forbidden"
					: pipe::complete_all_gpu_tuple_selected(dispatch_params)
						? "complete all-GPU tuple did not enter the dedicated resident route; generic CPU/SIMD biological fallback is forbidden"
						: "requested Vulkan components do not form a complete supported all-GPU tuple; generic CPU/SIMD biological fallback is forbidden";
				raise_vulkan_pg_error(
					"Vulkan dedicated route admission", detail);
			}

            auto& pool = bioseq::parallelism::GlobalPool::instance();
            /*
             * The persisted Format-6 scanner path historically serialized one
             * complete query at a time because its fragmented target could not
             * enter worker code. For a shared target, authenticate every
             * query/locator/reader identity on the PostgreSQL thread, copy the
             * packed target once under the existing admission authority, then
             * fan independent queries across the task pool. Workers borrow only
             * immutable resident sequence bytes, retained query sketches, and
             * the immutable selective reader; they never call PostgreSQL.
             *
             * TaskParallelRegion::query_batch owns the outer parallel region,
             * so every inner align_regions/segment region observes an active
             * TaskParallelScope and executes serially within its query worker.
             * Any failed preflight or resident admission falls through to the
             * established scanner_pg row loop below.
             */
            const bool may_parallel_persisted_native =
                persisted_required
                && use_scanner_pg
                && scanner_pg_ctx != nullptr
                && scanner_pg_ctx->explicit_query_ordinals
                && per_row_query_mode
                && per_row_query_hdrs != nullptr
                && !dispatch_params.enable_profiling
                && !selected_vulkan_accelerator(dispatch_params)
                && bioseq::parallelism::task_parallel(pool.mode())
                && pool.max_concurrency() > 1
                && batch_size
                    >= bioseq::parallelism::TaskParallelMinTriggers::
                        MIN_QUERY_COUNT
                && scanner_pg_ctx->cpu_prepared_target != nullptr;
            if (may_parallel_persisted_native) {
                auto* const prepared_target =
                    scanner_pg_ctx->cpu_prepared_target;
                bool batch_identity_valid =
                    prepared_target->unions_ready
                    && prepared_target->target_rowno >= 0
					&& prepared_target->query_count
						>= static_cast<std::uint64_t>(batch_size)
					&& (prepared_target->query_set_proof_ready()
						|| prepared_target->query_count
							== static_cast<std::uint64_t>(batch_size))
                    && prepared_target->target_bp_size
                        > static_cast<std::uint64_t>(
                            kPgSegmentationThresholdBp);
                const std::int64_t shared_target_rowno =
                    prepared_target->target_rowno;
                const std::uint64_t shared_target_bp_size =
                    prepared_target->target_bp_size;
                const bioseq::storage::seq_locator* shared_locator = nullptr;
                BatchProcessor::CpuPreparedIndexGroup* native_group = nullptr;
                std::vector<const BatchProcessor::CpuPreparedQuerySketch*>
                    retained_query_sketches(batch_size, nullptr);
                std::vector<Minimap2Params> native_query_params(batch_size);

                for (std::size_t row = 0;
                     batch_identity_valid && row < batch_size;
                     ++row) {
                    const pgseq_t* const row_query_hdr =
                        (*per_row_query_hdrs)[row];
                    const pgseq_t* const target_hdr = target_hdrs[row];
                    const auto& locator =
                        scanner_pg_ctx->target_locators[row];
                    if (row_query_hdr == nullptr
                        || row_query_hdr->alphabet_code != alphabet_code
                        || row_query_hdr->storage_unit_byte_width
                            != storage_unit_byte_width
                        || !locator.has_value()
                        || scanner_pg_ctx->target_rownos[row]
                            != shared_target_rowno
                        || (row != 0u
                            && scanner_pg_ctx->query_ordinals[row - 1u]
                                >= scanner_pg_ctx->query_ordinals[row])
                        || locator->total_bp_size != shared_target_bp_size
                        || (target_hdr != nullptr
                            && (target_hdr->alphabet_code != alphabet_code
                                || target_hdr->storage_unit_byte_width
                                    != storage_unit_byte_width
                                || target_hdr->bp_size
                                    != shared_target_bp_size))) {
                        batch_identity_valid = false;
                        break;
                    }
                    if (shared_locator == nullptr) {
                        shared_locator = &*locator;
                    } else if (!same_dispatch_target_locator(
                                   *shared_locator, *locator)) {
                        batch_identity_valid = false;
                        break;
                    }

                    const std::size_t min_bp = std::min<std::size_t>(
                        static_cast<std::size_t>(row_query_hdr->bp_size),
                        static_cast<std::size_t>(shared_target_bp_size));
                    Minimap2Params params = dispatch_params;
                    params.kmer_size = 15;
                    params.window_size = 10;
                    if (min_bp < 50u) {
                        params.min_chain_score = 10;
                        params.min_seed_count = 1;
                    } else if (min_bp < 200u) {
                        params.min_chain_score = 20;
                        params.min_seed_count = 2;
                    }
                    const auto backend =
                        pipe::target_index_sketch_backend_from_params(params);
                    auto* const group = prepared_target->find_group(
                        params.kmer_size,
                        params.window_size,
                        params.max_occ,
                        backend);
                    if (group == nullptr
                        || (native_group != nullptr
                            && native_group != group)) {
                        batch_identity_valid = false;
                        break;
                    }
                    native_group = group;
                    const auto* const retained =
                        group->find_retained_query_sketch(
                            scanner_pg_ctx->query_ordinals[row]);
                    if (retained == nullptr
                        || retained->query_bp_size
                            != static_cast<std::uint64_t>(
                                row_query_hdr->bp_size)
                        || !pipe::minimizer_range_proof_matches_backend(
                            backend,
                            retained->proof,
                            /*require_nonempty_vector_work=*/false,
                            pipe::ExactSimdRangeRoute::
                                require_continuation)) {
                        batch_identity_valid = false;
                        break;
                    }
                    retained_query_sketches[row] = retained;
                    native_query_params[row] = params;
                }

                batch_identity_valid =
                    batch_identity_valid
                    && shared_locator != nullptr
                    && native_group != nullptr
                    && (format6_parallel_reader_identity_valid(
                            *native_group, shared_target_bp_size)
                        || shape_v5_parallel_reader_identity_valid(
                            *native_group, shared_target_bp_size));
                if (batch_identity_valid) {
                    constexpr std::size_t letters_per_unit =
                        compact_sequence_range<S>::traits::letters_per_unit;
                    const std::uint64_t total_units =
                        (shared_target_bp_size + letters_per_unit - 1u)
                        / letters_per_unit;
                    const std::size_t padding = static_cast<std::size_t>(
                        total_units * letters_per_unit
                        - shared_target_bp_size);
                    using TargetBinding =
                        bioseq::storage::postgres_lazy_sequence_binding<S>;
                    const TargetBinding target_binding{
                        scanner_pg_ctx->db_oid,
                        scanner_pg_ctx->tablespace_oid,
                        scanner_pg_ctx->main_relfilenode,
                        scanner_pg_ctx->mmsearch_seq_attno,
                        *shared_locator,
                        padding,
                        scanner_pg_ctx->snapshot,
                        scanner_pg_ctx->caches,
                        scanner_pg_ctx->parent_mctx};
                    bool native_batch_completed = false;
                    bioseq::storage::with_lazy_postgres<S>(
                        target_binding,
                        [shared_target_rowno, shared_target_bp_size,
                         native_group, batch_size, &pool,
                         &native_query_params, &retained_query_sketches,
                         per_row_query_hdrs, scanner_pg_ctx, prepared_target,
                         record_target_operational_outcomes, &stats,
                         broadcast_query_ordinal,
                         &cached_query_minimizer_proof_operation_accounted,
                         &query_ids, &results, &native_batch_completed,
                         storage_unit_byte_width](
                                auto fragmented_target_view) {
                    auto resident =
                        try_materialize_target_for_segment_workers(
                            fragmented_target_view,
                            storage_unit_byte_width,
                            static_cast<std::uint64_t>(
                                shared_target_rowno));
                    if (resident.has_value()) {
                        const auto resident_target_view =
                            resident->owner.view();
                        const auto* const format6_reader =
                            native_group->persisted_format6_reader
                            ? &*native_group->persisted_format6_reader
                            : nullptr;
                        const auto* const shape_v5_reader =
                            native_group->persisted_shape_v5_reader
                            ? &*native_group->persisted_shape_v5_reader
                            : nullptr;
						const auto shape_v5_lookup_route =
							native_group->persisted_shape_v5_lookup_route;
						// Compact-direct preparation is itself the exact memory
						// authority. Attempt the complete canonical batch once,
						// without a duplicate occurrence-estimation pass. A typed
						// preparation-time admission failure may retry from wave 0
						// at the historical worker bound below.
						const std::size_t worker_bounded_shape_v5_wave_size =
							std::max(
								std::size_t{1u},
								std::min(
									batch_size,
									static_cast<std::size_t>(
										pool.max_concurrency())));
						const std::size_t shape_v5_wave_size =
							shape_v5_reader != nullptr
								&& shape_v5_lookup_route
									== bioseq::indexing::FragmentAnchorLookupRoute::
										compact_h32_direct
							? batch_size
							: worker_bounded_shape_v5_wave_size;
                        std::vector<MatchResultCollection>
                            native_collections(batch_size);
                        (void)run_aligner_pg_checked(
                            "persisted fragment-native parallel seed wave",
                            [&native_collections,
                             &native_query_params,
                             resident_target_view,
                             &retained_query_sketches,
                             per_row_query_hdrs,
							 format6_reader,
							 shape_v5_reader,
							 shape_v5_lookup_route,
							 prepared_target,
							 native_group,
							 batch_size,
							 worker_bounded_shape_v5_wave_size,
							 shape_v5_wave_size]()
                                    -> MatchResultCollection {
                                const auto align_query =
                                    [&native_collections,
                                     &native_query_params,
                                     resident_target_view,
                                     &retained_query_sketches,
                                     per_row_query_hdrs](
                                            const std::size_t row,
                                            const auto& target_index) {
                                        compact_sequence_range<S>
                                            query_seq{
                                                *(*per_row_query_hdrs)[row]};
                                        const auto lazy_query_view =
                                            bioseq::storage::
                                                as_lazy_sequence_view(
                                                    query_seq);
                                        native_collections[row] =
                                            minimap2_port_with_index(
                                                lazy_query_view,
                                                resident_target_view,
                                                retained_query_sketches[row]
                                                    ->minimizers,
                                                target_index,
                                                native_query_params[row],
                                                /*gpu_index_cache=*/nullptr,
                                                bioseq::indexing::
                                                    GpuIndexCacheIdentity{});
                                    };
                                if (format6_reader != nullptr) {
                                    bioseq::parallelism::
                                        parallel_for_pipeline(
                                            0,
                                            batch_size,
                                            1,
                                            bioseq::parallelism::
                                                TaskParallelMinTriggers::
                                                    MIN_QUERY_COUNT,
                                            bioseq::parallelism::
                                                TaskParallelRegion::
                                                    query_batch,
                                            [&align_query,
                                             format6_reader](
                                                    const std::size_t lo,
                                                    const std::size_t hi) {
                                                for (std::size_t row = lo;
                                                     row < hi;
                                                     ++row) {
                                                    align_query(
                                                        row,
                                                        *format6_reader);
                                                }
                                            });
                                    return {};
                                }
                                if (shape_v5_reader == nullptr) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                        "fragment-native Format-5 reader disappeared");
                                }

                                auto& memory_admission = bioseq::parallelism::
                                    MemoryAdmissionController::instance();
                                const std::uint64_t first_wave_prepare_started_ns =
                                    dispatch_host_now_ns();
                                auto scratch_result = bioseq::indexing::
                                    QueryWaveScratch::create(memory_admission);
                                if (!scratch_result) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                            "fragment-native query scratch was "
                                            "not admitted; wire_error="
                                            + std::to_string(
                                                static_cast<unsigned>(
                                                    scratch_result.error())));
                                }
                                // Reuse one admitted scratch owner across the
                                // serial waves. Each prepared batch owns its
                                // final anchors independently and is destroyed
                                // after its synchronous query wave completes.
                                auto query_wave_scratch =
                                    std::move(*scratch_result);

                                const std::uint64_t descriptor_bytes =
                                    bioseq::parallelism::saturating_mul(
                                        shape_v5_wave_size,
                                        sizeof(bioseq::indexing::
                                            FragmentSeedQuery));
                                const std::uint64_t descriptor_headroom =
                                    bioseq::parallelism::saturating_mul(
                                        descriptor_bytes,
                                        bioseq::parallelism::
                                            kVectorPreallocationHeadroomFactor);
                                if (descriptor_bytes
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()
                                    || descriptor_headroom
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                            "fragment-native query descriptor "
                                            "capacity overflow");
                                }
                                auto descriptor_permit_result =
                                    memory_admission.try_reserve(
                                        descriptor_headroom);
                                if (!descriptor_permit_result) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                            "fragment-native query descriptors "
                                            "were not admitted");
                                }
                                // Declared before the vector so reverse
                                // destruction releases storage before permit.
                                auto descriptor_permit =
                                    std::move(*descriptor_permit_result);
                                std::vector<bioseq::indexing::FragmentSeedQuery>
                                    wave_queries;
                                try {
                                    wave_queries.reserve(shape_v5_wave_size);
                                } catch (const std::bad_alloc&) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                            "fragment-native query descriptor "
                                            "allocation failed");
                                }
                                const std::uint64_t actual_descriptor_bytes =
                                    bioseq::parallelism::saturating_mul(
                                        wave_queries.capacity(),
                                        sizeof(bioseq::indexing::
                                            FragmentSeedQuery));
                                if (actual_descriptor_bytes
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()
                                    || (actual_descriptor_bytes
                                            > descriptor_permit.bytes()
                                        && !descriptor_permit.grow_to(
                                            actual_descriptor_bytes))) {
                                    throw bioseq::indexing::
                                        PersistedMinimizerLookupError(
                                            "fragment-native query descriptor "
                                            "reconciliation failed");
                                }
                                descriptor_permit.shrink_to(
                                    actual_descriptor_bytes);

								std::size_t active_shape_v5_wave_size =
									shape_v5_wave_size;
								bool full_wave_admission_retry_available =
									shape_v5_lookup_route
										== bioseq::indexing::
											FragmentAnchorLookupRoute::
												compact_h32_direct
									&& shape_v5_wave_size == batch_size
									&& worker_bounded_shape_v5_wave_size
										< batch_size;
                                for (std::size_t wave_begin = 0u;
									 wave_begin < batch_size;) {
                                    const std::uint64_t
                                        seed_wave_prepare_started_ns =
                                            wave_begin == 0u
                                            ? first_wave_prepare_started_ns
                                            : dispatch_host_now_ns();
                                    const std::size_t wave_end = std::min(
                                        batch_size,
										wave_begin
											+ active_shape_v5_wave_size);
                                    wave_queries.clear();
                                    for (std::size_t row = wave_begin;
                                         row < wave_end;
                                         ++row) {
                                        wave_queries.push_back(
                                            bioseq::indexing::FragmentSeedQuery{
                                                .minimizers =
                                                    retained_query_sketches[row]
                                                        ->minimizers,
                                                .query_length =
                                                    static_cast<sequence_size_t>(
                                                        (*per_row_query_hdrs)[row]
                                                            ->bp_size),
                                                .kmer_size =
                                                    native_query_params[row]
                                                        .kmer_size,
                                            });
                                    }
									bool retry_at_worker_bound = false;
									{
										FragmentAnchorWaveCompletion
											wave_completion{query_wave_scratch};
										std::optional<bioseq::indexing::
											PreparedFragmentAnchorBatch>
												prepared_seed_batch;
										try {
											prepared_seed_batch.emplace(
												shape_v5_reader->
													prepare_fragment_anchor_batch(
														memory_admission,
														std::span<const bioseq::indexing::
															FragmentSeedQuery>{
																wave_queries},
														native_query_params[
															wave_begin].max_occ,
														query_wave_scratch,
														/*baseline_query_plan_matcher_for_testing=*/false,
														/*verify_compositional_order_for_testing=*/false,
														shape_v5_lookup_route));
										} catch (const bioseq::indexing::
												PersistedMinimizerLookupError& error) {
											retry_at_worker_bound =
												wave_begin == 0u
												&& full_wave_admission_retry_available
												&& error.reason()
													== bioseq::indexing::
														PersistedMinimizerLookupError::
															Reason::
																admission_or_allocation;
										if (!retry_at_worker_bound) throw;
									}

									if (!retry_at_worker_bound) {
										if (native_group->
												compact_direct_query_set_proof_pending) {
											const bool complete_query_set =
												wave_begin == 0u
												&& wave_end == batch_size
												&& wave_queries.size() == batch_size;
											if (!complete_query_set
												|| !BatchProcessor::
													publish_compact_direct_query_set_proof(
														*prepared_target,
														*native_group,
														prepared_seed_batch->
															compact_direct_query_set_proof())) {
												throw bioseq::indexing::
													PersistedMinimizerLookupError(
														"compact-direct full-batch query-set proof was rejected");
											}
										}
										const std::uint64_t
												seed_wave_prepare_host_wall_ns =
													elapsed_dispatch_host_ns(
														seed_wave_prepare_started_ns);
											bioseq::parallelism::
												parallel_for_pipeline(
													wave_begin,
													wave_end,
													1,
													bioseq::parallelism::
														TaskParallelMinTriggers::
															MIN_QUERY_COUNT,
													bioseq::parallelism::
														TaskParallelRegion::
															query_batch,
													[&align_query,
													 &prepared_seed_batch,
													 wave_begin](
															const std::size_t lo,
															const std::size_t hi) {
														for (std::size_t row = lo;
															 row < hi;
															 ++row) {
															const auto query_index =
																prepared_seed_batch->
																	query_view(
																		row
																			- wave_begin);
															align_query(
																row,
																query_index);
														}
													});
											native_collections[wave_begin].
												record_fragment_native_query_evidence(
													prepared_seed_batch->evidence());
											native_collections[wave_begin].
												record_seed_lookup_host_time(
													seed_wave_prepare_host_wall_ns);
										}
									}
									if (retry_at_worker_bound) {
										full_wave_admission_retry_available = false;
										if (query_wave_scratch.wave_outstanding()) {
											throw bioseq::indexing::
												PersistedMinimizerLookupError(
													"fragment-native failed wave retained "
													"active scratch state");
										}
										query_wave_scratch.
											release_retained_capacity();
										BatchProcessor::
											finalize_pending_compact_direct_query_set_proof_fallback(
												*prepared_target);
										active_shape_v5_wave_size =
											worker_bounded_shape_v5_wave_size;
										wave_queries.clear();
										continue;
									}
									wave_begin = wave_end;
                                }
                                return {};
                            });

                        if (!native_collections.empty()
                            && native_group->persisted_reader_queries == 0u
                            && native_group->
                                persisted_reader_load_host_wall_ns != 0u) {
                            native_collections.front().
                                record_persisted_index_decode_host_time(
                                    native_group->
                                        persisted_reader_load_host_wall_ns);
                        }
                        for (std::size_t row = 0;
                             row < batch_size;
                             ++row) {
                            attach_retained_format6_query_proof(
                                native_collections[row],
                                retained_query_sketches[row]->proof);
                            scanner_pg_ctx->target_index_sources[row] =
                                ::bioseq::pipeline_stats::
                                    TargetIndexSource::persisted;
                                scanner_pg_ctx->persisted_generation_ids[row] =
                                native_group->persisted_generation_id;
                                scanner_pg_ctx->persisted_source_identities[row] =
                                native_group->persisted_source_identity;
                        }
                        native_group->persisted_reader_queries +=
                            static_cast<std::uint64_t>(batch_size);

                        const auto requested_source =
                            requested_target_index_source(scanner_pg_ctx);
                        for (std::size_t row = 0;
                             row < batch_size;
                             ++row) {
                            bioseq::pg_utils::invoke_pg_as_cpp(
                                [] { CHECK_FOR_INTERRUPTS(); });
                            if (record_target_operational_outcomes) {
                                record_target_row(stats);
                                record_target_index_outcome(
                                    stats,
                                    requested_source,
                                    ::bioseq::pipeline_stats::
                                        TargetIndexSource::persisted);
                            }
                            record_unattested_job(
                                stats,
                                native_query_params[row],
                                requested_source,
                                ::bioseq::pipeline_stats::
                                    TargetIndexSource::persisted,
                                scanner_pg_ctx,
                                row,
                                /*per_row_query_mode=*/true,
                                broadcast_query_ordinal,
                                &native_collections[row],
                                /*consumed_shared_cached_query_proof=*/false,
                                cached_query_minimizer_proof_operation_accounted);
                            bioseq_log<DBG>(
                                "[bioseq] pg: BatchProcessor row query_id={} "
                                "target_bp_size={} k={} w={} min_chain_score={} "
                                "min_seed_count={} prebuilt_index=true produced "
                                "{} match(es) [fragment-native-query-parallel]",
                                static_cast<int>(query_ids[row]),
                                shared_target_bp_size,
                                native_query_params[row].kmer_size,
                                native_query_params[row].window_size,
                                native_query_params[row].min_chain_score,
                                native_query_params[row].min_seed_count,
                                native_collections[row].size());
                            if (!native_collections[row].empty()) {
                                results[query_ids[row]] =
                                    std::move(
                                        native_collections[row].results);
                            }
                        }
                        native_batch_completed = true;
                    }
                    });
                    if (native_batch_completed) return results;
                }
			}
			if (scanner_pg_ctx != nullptr
				&& scanner_pg_ctx->cpu_prepared_target != nullptr
				&& !scanner_pg_ctx->cpu_prepared_target->
					query_set_proof_ready()) {
				(void)run_aligner_pg_checked(
					"compact-direct query-set slow fallback",
					[prepared_target = scanner_pg_ctx->cpu_prepared_target]()
							-> MatchResultCollection {
						BatchProcessor::
							finalize_pending_compact_direct_query_set_proof_fallback(
								*prepared_target);
						return {};
					});
			}

			std::vector<std::uint8_t> parallel_done;
            std::vector<MatchResultCollection> parallel_collections;
            std::vector<Minimap2Params> parallel_params;

            const bool may_dispatch_parallel =
                !dispatch_params.enable_profiling
                && !selected_vulkan_accelerator(dispatch_params)
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
				bioseq::pg_utils::invoke_pg_as_cpp(
					[] { CHECK_FOR_INTERRUPTS(); });
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
						const int query_id = static_cast<int>(query_ids[i]);
						bioseq::pg_utils::invoke_pg_as_cpp(
							[query_id, target_bp_size] {
								ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("forced in-memory scanner cannot materialize target stub"),
									 errdetail("row=%d target_bp=%llu exceeds MaxAllocSize",
										query_id,
										static_cast<unsigned long long>(target_bp_size)),
									 errhint("Set bioseq.test_force_inmemory_scanner=off to use the streaming scanner_pg path.")));
							});
						pg_unreachable();
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
