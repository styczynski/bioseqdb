/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor.cppm
 *
 * Heavy minimap/pipe implementation for BatchProcessor. Kept out of the
 * dispatch_buffer interface so edits to tuple-buffer or admin code do not
 * force that translation unit to reinstantiate the full supported alphabet x
 * storage-width minimap pipeline.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>
#include "common.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module bioseq_scheduler.dispatch_processor;

import mlogging;
import pg_utils;
import bioseq_indexing.type;
import bioseq_indexing.minimizer_hashmap;
import bioseq_indexing.minimizer_sequence;
import bioseq_storage_indexing_pg;
import bioseq_storage_indexing_handles;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.minimap_port;
import bioseq_sequences.pipe;
import bioseq_types.pg_compact_sequence;
import bioseq_instr;
import bioseq_storage;
import bioseq_storage_lazy;
import bioseq_storage_scanner;
import bioseq_storage_scanner_pg;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

export constexpr size_t kDispatchBatchCapacity = 1000;

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
     * The implementation lives in dispatch_processor.cppm so this module
     * interface does not instantiate the full pgseq_supported_alphabets ×
     * storage-width minimap pipeline whenever tuple-buffer/admin code changes.
     */
    [[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const pgseq_t* query_hdr,
        const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
        const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
        const Minimap2Params& base_params,
        BatchExecutionStats& stats,
        ScannerPgContext* scanner_pg_ctx = nullptr);

};

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
    /*
     * Only scale when every relevant parameter still matches the minimap2
     * default. If the caller has already tuned one of these fields, we
     * trust them and leave the batch alone.
     */
    const bool all_defaults =
        base.kmer_size       == 15 &&
        base.window_size     == 10 &&
        base.min_chain_score == 40 &&
        base.min_seed_count  == 3;
    if (!all_defaults) {
        return base;
    }
    Minimap2Params scaled = base;
    if (min_bp < 50) {
        scaled.kmer_size       = 8;
        scaled.window_size     = 3;
        scaled.min_chain_score = 10;
        scaled.min_seed_count  = 1;
    } else if (min_bp < 200) {
        scaled.kmer_size       = 10;
        scaled.window_size     = 5;
        scaled.min_chain_score = 20;
        scaled.min_seed_count  = 2;
    }
    return scaled;
}

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> BatchProcessor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchExecutionStats& stats,
    ScannerPgContext* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<MatchResult>> results;

    if (query_hdr == nullptr) {
        JustLog("[bioseq] pg: BatchProcessor::process_sequences received null query_hdr; returning empty map");
        return results;
    }
    if (query_hdr->alphabet_code != alphabet_code || query_hdr->storage_unit_byte_width != storage_unit_byte_width) {
        JustLog("[bioseq] pg: BatchProcessor::process_sequences query_hdr typmod ({},{}) disagrees with context ({},{}); refusing to dispatch",
                static_cast<unsigned>(query_hdr->alphabet_code),
                static_cast<unsigned>(query_hdr->storage_unit_byte_width),
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
        return results;
    }

    /*
     * Profiler dispatch fork. Picks one of the two pipelines based
     * on bioseq.aligner.enable_profiling (mirrored into
     * Minimap2Params::enable_profiling by build_params_from_gucs in
     * columnar_customscan.cppm). The two compile to genuinely
     * different code: every templated bioseq::instr::region/loop
     * call below is instantiated per Args, so the no-profiling
     * instantiation collapses to nothing.
     *
     * We use the run_pipeline lambda + a per-args constexpr-if dispatcher
     * to share the pipeline body; the templated outer scope owns the
     * ProfilerBlock + RAII installer when profiling is on.
     */
    auto run_pipeline_body =
        [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
         &target_hdrs, &query_ids, &base_params, &stats, scanner_pg_ctx,
         &results]<bioseq::instr::is_context_args Args>() {
        static const auto* batch_desc =
            bioseq::instr::register_region_static("BatchProcessor.process_sequences");
        auto _batch_scope = bioseq::instr::region<Args>(batch_desc);

        pgseq_t::dispatch_instance(alphabet_code, storage_unit_byte_width,
            [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
             &target_hdrs, &query_ids, &base_params, &stats, scanner_pg_ctx,
             &results]<class S>() {
            /*
             * Build the per-batch query compact_sequence_range once
             * from the stashed PG varlena. The DOC0258 view
             * specialisation is reference-only — no allocation
             * happens here. The bytes inside `query_hdr` outlive
             * the batch via the buffer's std::vector<char>.
             */
            compact_sequence_range<S> query_seq{*query_hdr};
            // DOC0263 §9.7 — present the row sequence to the migrated
            // pipeline through the zero-overhead no-fragment lazy wrapper.
            auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
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
            JustLog("[bioseq] pg: BatchProcessor dispatching ({},{}) query_bp_size={} batch_size={} use_scanner_pg={} chunked={} inmemory_forced={}",
                    static_cast<unsigned>(alphabet_code),
                    static_cast<unsigned>(storage_unit_byte_width),
                    query_seq.bp_size(), batch_size,
                    use_scanner_pg, chunked_scanner_forced, inmemory_scanner_forced);

            /*
             * Query minimizers are computed once per batch for the
             * prebuilt-index path (every row reuses the same query).
             * Recompute-path rows use minimap_port(), which does its
             * own sketching; the extra allocation here is avoided
             * for those rows via the optional nested-scope.
             */
            std::optional<std::vector<MinimapSketchData>> cached_query_minimizers;

            for (size_t i = 0; i < batch_size; ++i) {
                // DOC0276 — cancellation point at the per-row boundary. The
                // native aligner itself is NO_POSTGRES and cannot call
                // CHECK_FOR_INTERRUPTS; this lets a multi-row mmsearch honour
                // pg_cancel_backend between rows, and (with the storage-layer
                // check in fetch_payload) bounds how long a cancel waits.
                CHECK_FOR_INTERRUPTS();
                JustLogProgress("[dispatch] mmsearch row {}/{} start"
                        " | footprint={}MB avail={}MB",
                        i + 1, batch_size,
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
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
                    JustLog("[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
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

                // DOC0276 — targets larger than this recompute via the
                // memory-bounded segmented index (`minimap_port_segmented`,
                // multiple ≤200 Mbp segments). At/below it the size-bucket
                // `minimap_port` fits RAM, so small-target results are
                // unchanged. 256 Mbp ≈ the 2^28 MinimapSketchData position
                // boundary where the size-bucket would otherwise widen.
                constexpr sequence_size_t kSegmentationThresholdBp = 256'000'000;

                const size_t min_bp = std::min<size_t>(
                    static_cast<size_t>(query_seq.bp_size()),
                    static_cast<size_t>(target_bp_size));

                ++stats.targets_considered;

                MatchResultCollection collection;
                {
                    ++stats.targets_recomputed_minimizers;
                    Minimap2Params params = apply_size_heuristic(base_params, min_bp);
                    if (use_scanner_pg) {
                        // DOC0264 §9.2 / DOC0274 — production scanner_pg path.
                        using SP = bioseq::storage::sequence_scanner_pg<S>;
                        bioseq::storage::lazy_compact_sequence<S, SP> fq_pg{
                            bioseq::storage::make_scanner_pg_in_memory<S>(query_seq)};
                        const auto& loc_opt = scanner_pg_ctx->target_locators[i];
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
                                scanner_pg_ctx->caches,
                                scanner_pg_ctx->parent_mctx);
                            bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
                                std::move(target_scanner)};
                            // DOC0279 §11.3a — ASSEMBLE from the stored
                            // per-fragment minimizer index if present
                            // (deserialise + globalise, no re-collection);
                            // else fall back to recompute (DOC0276).
                            bool assembled = false;
                            if (scanner_pg_ctx->read_index_active
                                && OidIsValid(scanner_pg_ctx->user_relid)) {
                                using IStore = bioseq::storage::indexing::IndexStorageFragmentPg;
                                const bool want_seq =
                                    scanner_pg_ctx->active_index == "minimizers_per_sequence";
                                // The stored index is ALWAYS w=10,k=15 (pinned at build); the query
                                // minimizers + the seed pipeline MUST use the same w,k (NOT the size
                                // heuristic's reduced k), exactly as the legacy prebuilt branch does.
                                Minimap2Params idx_params = base_params;
                                idx_params.kmer_size = 15; idx_params.window_size = 10;
                                if (min_bp < 50) { idx_params.min_chain_score = 10; idx_params.min_seed_count = 1; }
                                else if (min_bp < 200) { idx_params.min_chain_score = 20; idx_params.min_seed_count = 2; }
                                auto ensure_q =
                                    [&cached_query_minimizers, &query_seq, &idx_params] {
                                    if (!cached_query_minimizers.has_value()) {
                                        sketch_collector qc;
                                        collect_minimizers<minimap_int_hasher, MinimapSketchData>(
                                            qc, query_seq, query_seq.begin(), query_seq.end(),
                                            static_cast<size_t>(idx_params.window_size),
                                            static_cast<size_t>(idx_params.kmer_size));
                                        cached_query_minimizers.emplace(std::move(qc).extract_sketches_data());
                                    }
                                };
                                // DOC0279 §11.3 — route by the table's ACTIVE read index
                                // (set_active_read_index / resolve_active_read_index, default
                                // minimizers_per_fragment). Per-row presence still gates: an
                                // active index not materialised for THIS row recomputes.
                                if (want_seq) {
                                // §11.3b — per-sequence merged index: query-relevant buckets
                                // only + exact global max_occ.
                                auto seq_store = bioseq::storage::indexing::open_fragment_storage(
                                    scanner_pg_ctx->user_relid, scanner_pg_ctx->db_oid,
                                    scanner_pg_ctx->tablespace_oid, scanner_pg_ctx->main_relfilenode,
                                    scanner_pg_ctx->mmsearch_seq_attno, "minimizers_per_sequence",
                                    *loc_opt, (uint64_t)scanner_pg_ctx->target_rownos[i],
                                    scanner_pg_ctx->caches, scanner_pg_ctx->parent_mctx);
                                if (seq_store.number_of_chunks(bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
                                    ensure_q();
                                    bioseq::indexing::MinimizerAssemblyScratch sq_scratch;
                                    auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<IStore>::assemble_for_query(
                                        seq_store,
                                        std::span<const MinimapSketchData>(cached_query_minimizers->data(),
                                                                           cached_query_minimizers->size()),
                                        loc_opt->total_bp_size, idx_params.max_occ, sq_scratch);
                                    collection = minimap2_port_with_index(fq_pg, ft_pg, *cached_query_minimizers, idx, idx_params);
                                    assembled = true;
                                }
                                } else {
                                auto frag_store = bioseq::storage::indexing::open_fragment_storage(
                                    scanner_pg_ctx->user_relid, scanner_pg_ctx->db_oid,
                                    scanner_pg_ctx->tablespace_oid, scanner_pg_ctx->main_relfilenode,
                                    scanner_pg_ctx->mmsearch_seq_attno, "minimizers_per_fragment",
                                    *loc_opt, (uint64_t)scanner_pg_ctx->target_rownos[i],
                                    scanner_pg_ctx->caches, scanner_pg_ctx->parent_mctx);
                                if (frag_store.number_of_chunks(bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
                                    ensure_q();
                                    const sequence_size_t total_bp = loc_opt->total_bp_size;
                                    const std::size_t frag_units = SP::fragment_storage_units;
                                    const std::size_t a_lpu = compact_sequence_range<S>::traits::letters_per_unit;
                                    const sequence_size_t frag_bp = (sequence_size_t)frag_units * a_lpu;
                                    const std::size_t size_u = (std::size_t)((total_bp + a_lpu - 1) / a_lpu);
                                    bioseq::indexing::MinimizerAssemblyScratch scratch;
                                    auto assemble_one =
                                        [frag_units, a_lpu, total_bp, frag_bp,
                                         &frag_store, &scratch](std::size_t cs_u, std::size_t ce_u) {
                                        const std::size_t frag_lo = cs_u / frag_units;
                                        const std::size_t frag_hi = (ce_u + frag_units - 1) / frag_units;
                                        const sequence_size_t cs_bp = (sequence_size_t)cs_u * a_lpu;
                                        const sequence_size_t ce_bp = std::min((sequence_size_t)ce_u * a_lpu, total_bp);
                                        return bioseq::indexing::MinimizersPerFragmentIndex<IStore>::assemble_segment(
                                            frag_store, frag_lo, frag_hi, frag_bp, total_bp, cs_bp, ce_bp, scratch);
                                    };
                                    if (total_bp <= kSegmentationThresholdBp) {
                                        auto seg = assemble_one(0, size_u);
                                        collection = minimap2_port_with_index(fq_pg, ft_pg, *cached_query_minimizers, seg, idx_params);
                                    } else {
                                        std::vector<MatchResult> merged;
                                        const sequence_size_t query_len = query_seq.bp_size();
                                        for (auto sr : pipe::segment_unit_ranges(total_bp, query_len, a_lpu)) {
                                            auto seg = assemble_one(sr.cs_u, sr.ce_u);
                                            MatchResultCollection seg_res =
                                                minimap2_port_with_index(fq_pg, ft_pg, *cached_query_minimizers, seg, idx_params);
                                            for (auto& r : seg_res.results)
                                                if (r.target_start >= sr.core_lo_bp && r.target_start < sr.core_hi_bp)
                                                    merged.push_back(std::move(r));
                                        }
                                        collection = MatchResultCollection{std::move(merged)};
                                    }
                                    assembled = true;
                                }
                                }  // close the per-fragment else branch
                            }
                            if (!assembled) {
                                collection =
                                    (target_bp_size > kSegmentationThresholdBp)
                                        ? minimap_port_segmented(fq_pg, ft_pg, params)
                                        : minimap_port(fq_pg, ft_pg, params);
                            }
                        } else {
                            compact_sequence_range<S> target_seq{*target_hdr};
                            bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
                                bioseq::storage::make_scanner_pg_in_memory<S>(target_seq)};
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(fq_pg, ft_pg, params)
                                    : minimap_port(fq_pg, ft_pg, params);
                        }
                    } else if (target_hdr != nullptr) {
                        compact_sequence_range<S> target_seq{*target_hdr};
                        auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
                        if (chunked_scanner_forced) {
                            using chunked = bioseq::storage::sequence_scanner_test_chunked<
                                typename compact_sequence_range<S>::traits::storage_unit,
                                bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
                            bioseq::storage::lazy_compact_sequence<S, chunked> fq{chunked::from_range(query_seq)};
                            bioseq::storage::lazy_compact_sequence<S, chunked> ft{chunked::from_range(target_seq)};
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(fq, ft, params)
                                    : minimap_port(fq, ft, params);
                        } else {
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(lazy_query, lazy_target, params)
                                    : minimap_port(lazy_query, lazy_target, params);
                        }
                    } else if (scanner_pg_ctx != nullptr
                               && scanner_pg_ctx->target_locators[i].has_value()) {
                        // DOC0274 (round-2 revert): inmemory_forced=ON cannot satisfy its
                        // force-baseline semantic on > MaxAllocSize stubs; raise.
                        pg_log(ERROR,
                            "bioseq.test_force_inmemory_scanner=on: row {} target stub "
                            "({} bp) exceeds MaxAllocSize and cannot be materialised "
                            "for the Phase B baseline. Either set the GUC to off (use "
                            "the Phase C scanner_pg path which streams fragments) or "
                            "wait for Phase D D-VARLENA. See DOC0267.",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned long long>(target_bp_size));
                    } else {
                        JustLog("[bioseq] pg: row {} has no inline target and no scanner_pg locator; treating as WHERE FALSE",
                                static_cast<int>(query_ids[i]));
                        continue;
                    }
                    stats.last_effective_params = params;
                }

                JustLog("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
                        static_cast<int>(query_ids[i]),
                        target_bp_size,
                        stats.last_effective_params.kmer_size,
                        stats.last_effective_params.window_size,
                        stats.last_effective_params.min_chain_score,
                        stats.last_effective_params.min_seed_count,
                        stats.targets_with_prebuilt_index, collection.size());
                if (!collection.empty()) {
                    results[query_ids[i]] = std::move(collection.results);
                }
            }
        });
    };  // run_pipeline_body

    if (base_params.enable_profiling) {
        /*
         * Profiling pipeline. The ProfilerBlock is the persistent
         * process-wide singleton (see global_state.cppm); aggregates
         * accumulate across every batch in the current scan. The
         * one-shot summary fires at scan end inside
         * ColumnarScan_EndCustomScan (Part 48.5/48.8 of the
         * design doc) — emitting it here would produce one summary
         * per batch and accumulate duplicates on multi-batch scans.
         */
        // Install both the full (with PMU counters) and timing-only
        // profiler blocks. The non-templated `region(d)` / `loop(d)`
        // call sites in chaining / kmers / minimap_port are pinned to
        // context_args_profiling_timing_only so they short-circuit
        // unless we also install that block here. Postgres production
        // monitoring continues to use the full block (counters on)
        // for any explicitly-templated `region<full_args>(d)` calls.
        using Args = bioseq::instr::context_args_profiling_full;
        using TimingArgs = bioseq::instr::context_args_profiling_timing_only;
        auto& profiler_block =
            bioseq::instr::persistent_profiler_block<Args>();
        auto& timing_profiler_block =
            bioseq::instr::persistent_profiler_block<TimingArgs>();
        bioseq::instr::installed_profiler_scope<Args> guard(profiler_block);
        bioseq::instr::installed_profiler_scope<TimingArgs> timing_guard(timing_profiler_block);
        run_pipeline_body.template operator()<Args>();
    } else {
        using Args = bioseq::instr::context_args_no_profiling;
        run_pipeline_body.template operator()<Args>();
    }

    return results;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
