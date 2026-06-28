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
module;

#include <hwy/highway.h>
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

export module bioseq.scheduler.dispatch_processor;

import bioseq.common.mlogging;
import bioseq.common.hashing;
import bioseq.common.sequan_compat;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_index_run_buffer_bytes, bioseq_scheduler_*, bioseq_index_memory_cap_mb GUCs
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.minimizer_vulkan;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;
import bioseq.storage.indexing_pg.residency;
import bioseq.storage.indexing_pg.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan;   // DOC0345 Patch J — GPU sorted-pair segment seam (GPU emit + host sort; real no_index mps)
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.types.pg_compact_sequence;
import bioseq.instr;
import bioseq.storage.manager;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.storage.scanner_pg;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.scheduler.supervisor_pg;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

export constexpr size_t kDispatchBatchCapacity = 8192;
/*
 * BatchProcessor drives minimap_port across a batch of (query, target)
 * pairs that all share the same (alphabet_code, storage_unit_byte_width) typmod. The
 * caller hands us the already-detoasted query varlena (captured at
 * BeginCustomScan — see columnar_customscan.cppm) and an array of per-row
 * target varlenas (captured at submit_scan — see TupleDispatchBuffer).
 *
 * Dispatch: we branch on (alphabet_code, storage_unit_byte_width) exactly once, then
 * inside the matching arm all eight compact_sequence_range<Dst>
 * specialisations compile down to their own minimap_port instantiation.
 * No std::variant, no runtime type erasure on the hot path.
 */
/*
 * Execution counters captured while BatchProcessor walks a batch. Used by
 * EXPLAIN to print an accurate "Recomputed minimizers: X%" estimate.
 * The struct lives next to the BatchProcessor instance inside the
 * TupleDispatchBuffer so EXPLAIN can read it after run_batch_and_build_cursors
 * without doing a second walk.
 */
export struct GpuBatchExecutionStats {
    bool gpu_batched_branch_taken = false;
    std::uint64_t gpu_problem_count = 0;
    std::uint64_t gpu_wave_count = 0;
    std::uint64_t gpu_segment_indexes_built = 0;
    std::uint64_t gpu_segment_indexes_reused = 0;
    std::uint64_t gpu_seed_dispatches = 0;
    std::uint64_t gpu_chain_dispatches = 0;
    std::uint64_t gpu_extend_dispatches = 0;
    std::uint64_t cpu_fallback_rows = 0;
    std::uint64_t simd_fallback_rows = 0;
    std::uint64_t vulkan_declines = 0;
    std::uint64_t gpu_batch_pipeline_ns = 0;
    std::uint64_t gpu_query_sketch_ns = 0;
    std::uint64_t gpu_target_dedup_ns = 0;
    std::uint64_t gpu_target_plan_ns = 0;
    std::uint64_t gpu_index_build_ns = 0;
    std::uint64_t gpu_problem_build_ns = 0;
    std::uint64_t gpu_seed_chain_runtime_ns = 0;
    std::uint64_t gpu_cigar_jobs = 0;
    std::uint64_t gpu_cigar_bounded_jobs = 0;
    std::uint64_t gpu_cigar_exact_jobs = 0;
    std::uint64_t gpu_cigar_unit_gap_jobs = 0;
    std::uint64_t gpu_cigar_gridded_jobs = 0;
    std::uint64_t gpu_cigar_query_symbols = 0;
    std::uint64_t gpu_cigar_target_symbols = 0;
    std::uint64_t gpu_cigar_packed_words = 0;
    std::uint64_t gpu_cigar_traceback_words = 0;
    std::uint64_t gpu_cigar_prepare_ns = 0;
    std::uint64_t gpu_cigar_symbol_pack_ns = 0;
    std::uint64_t gpu_cigar_classify_ns = 0;
    std::uint64_t gpu_cigar_runtime_ns = 0;
    std::uint64_t gpu_cigar_decode_ns = 0;

    void merge(const GpuBatchExecutionStats& rhs) noexcept {
        gpu_batched_branch_taken =
            gpu_batched_branch_taken || rhs.gpu_batched_branch_taken;
        gpu_problem_count += rhs.gpu_problem_count;
        gpu_wave_count += rhs.gpu_wave_count;
        gpu_segment_indexes_built += rhs.gpu_segment_indexes_built;
        gpu_segment_indexes_reused += rhs.gpu_segment_indexes_reused;
        gpu_seed_dispatches += rhs.gpu_seed_dispatches;
        gpu_chain_dispatches += rhs.gpu_chain_dispatches;
        gpu_extend_dispatches += rhs.gpu_extend_dispatches;
        cpu_fallback_rows += rhs.cpu_fallback_rows;
        simd_fallback_rows += rhs.simd_fallback_rows;
        vulkan_declines += rhs.vulkan_declines;
        gpu_batch_pipeline_ns += rhs.gpu_batch_pipeline_ns;
        gpu_query_sketch_ns += rhs.gpu_query_sketch_ns;
        gpu_target_dedup_ns += rhs.gpu_target_dedup_ns;
        gpu_target_plan_ns += rhs.gpu_target_plan_ns;
        gpu_index_build_ns += rhs.gpu_index_build_ns;
        gpu_problem_build_ns += rhs.gpu_problem_build_ns;
        gpu_seed_chain_runtime_ns += rhs.gpu_seed_chain_runtime_ns;
        gpu_cigar_jobs += rhs.gpu_cigar_jobs;
        gpu_cigar_bounded_jobs += rhs.gpu_cigar_bounded_jobs;
        gpu_cigar_exact_jobs += rhs.gpu_cigar_exact_jobs;
        gpu_cigar_unit_gap_jobs += rhs.gpu_cigar_unit_gap_jobs;
        gpu_cigar_gridded_jobs += rhs.gpu_cigar_gridded_jobs;
        gpu_cigar_query_symbols += rhs.gpu_cigar_query_symbols;
        gpu_cigar_target_symbols += rhs.gpu_cigar_target_symbols;
        gpu_cigar_packed_words += rhs.gpu_cigar_packed_words;
        gpu_cigar_traceback_words += rhs.gpu_cigar_traceback_words;
        gpu_cigar_prepare_ns += rhs.gpu_cigar_prepare_ns;
        gpu_cigar_symbol_pack_ns += rhs.gpu_cigar_symbol_pack_ns;
        gpu_cigar_classify_ns += rhs.gpu_cigar_classify_ns;
        gpu_cigar_runtime_ns += rhs.gpu_cigar_runtime_ns;
        gpu_cigar_decode_ns += rhs.gpu_cigar_decode_ns;
    }
};

export struct BatchExecutionStats {
    /// Total rows submitted to process_sequences().
    size_t targets_considered = 0;
    /// Rows that arrived with a prebuilt index pointer and reused it.
    size_t targets_with_prebuilt_index = 0;
    /// Rows whose minimizers had to be recomputed from the compact_sequence_range.
    size_t targets_recomputed_minimizers = 0;
    /// Effective parameters used for the last batch. Captured for EXPLAIN so
    /// the user sees the actually-applied values after overrides + size-heuristic
    /// adjustments.
    Minimap2Params last_effective_params{};
    /// Selected-Vulkan proof counters surfaced through EXPLAIN/bench reports.
    GpuBatchExecutionStats gpu{};

    [[nodiscard]] float recomputed_fraction() const {
        return targets_considered == 0
            ? 0.0f
            : static_cast<float>(targets_recomputed_minimizers)
              / static_cast<float>(targets_considered);
    }
};

export class BatchProcessor {
public:
    /*
     * DOC0264 / DOC0274 — context required to wire scanner_pg into the
     * per-row pipeline dispatch. When non-null, the dispatch loop uses
     * `sequence_scanner_pg<S>` (PG-backed for stub-backed targets, in-memory
     * for the query literal). When null, the dispatch loop falls back to
     * the Phase B baseline `with_lazy_in_memory`.
     */
    struct ScannerPgContext {
        Oid                                              db_oid               = InvalidOid;
        Oid                                              tablespace_oid       = InvalidOid;
        Oid                                              main_relfilenode     = InvalidOid;
        std::int16_t                                     mmsearch_seq_attno   = 0;
        bioseq::storage::seq_caches*                     caches               = nullptr;
        MemoryContext                                    parent_mctx          = nullptr;
        // Per-row optional locator. nullopt means "use the inline pgseq_t at
        // target_hdrs[i]" — scanner_pg is then constructed in in-memory mode.
        std::array<std::optional<bioseq::storage::seq_locator>,
            kDispatchBatchCapacity>                       target_locators{};
        // DOC0279 §11.3c — per-row columnar rowno (== index query_key) and the
        // user relation oid, so the stored per-fragment index can be assembled.
        std::array<std::int64_t, kDispatchBatchCapacity>  target_rownos{};
        Oid                                               user_relid           = InvalidOid;
        bool                                              read_index_active    = false;
        bool                                              index_async_persist_active = false;
        bool                                              recompute_async_persist_active = false;
        // DOC0279 §11.3/§12.3 — the table's ACTIVE read index for the target
        // column (resolve_active_read_index), default minimizers_per_fragment.
        std::string                                       active_index;
    };

    /*
     * Run the minimap2 pipeline across a batch. DOC0279 §11.3 — per target
     * row, the loop ASSEMBLES the table's active per-sequence minimizer index
     * from its chunked storage (gated by read_index_active + per-row presence)
     * or RECOMPUTES it; the two are mixed freely within one batch.
     *
     * DOC0264 / DOC0274 — production dispatch posture.
     *
     * The Phase C `sequence_scanner_pg<S>` (in module
     * `bioseq_storage_scanner_pg`) is now wired into the dispatch loop.
     * `scanner_pg_ctx` carries the per-batch identity (db/spc/relfilenode/
     * attno/seq_caches/parent_mctx) plus the per-row optional locator the
     * caller decoded at submit_scan time. When `scanner_pg_ctx` is non-null
     * AND the user has not forced the in-memory baseline via
     * `bioseq.test_force_inmemory_scanner = on`, the loop dispatches:
     *   - target through `sequence_scanner_pg<S>` (PG-backed when locator
     *     present; in-memory wrap of the inline pgseq_t otherwise);
     *   - query through `sequence_scanner_pg<S>` in-memory wrap of the
     *     batch's query literal.
     * Both share the same `Scanner = sequence_scanner_pg<S>` template, so
     * the pipeline tree adds one instantiation (alongside the existing
     * `sequence_scanner_none` and `sequence_scanner_test_chunked` trees).
     *
     * NOTE on parameter passing: `target_hdrs` and `query_ids` are full-size
     * `kDispatchBatchCapacity`-element arrays passed by const-ref so the call
     * does not copy them. The result map is RVO'd / moved on return.
     */
    /*
     * Run the minimap2 pipeline across the fixed dispatch-buffer batch shape.
     * The implementation lives in dispatch_processor_impl.cpp so this module
     * interface does not instantiate the full pgseq_supported_alphabets ×
     * storage-width minimap pipeline whenever tuple-buffer/admin code changes.
     */
    [[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const pgseq_t* query_hdr,
        const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
        const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
        const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
        const Minimap2Params& base_params,
        BatchExecutionStats& stats,
        ScannerPgContext* scanner_pg_ctx = nullptr);

};


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
