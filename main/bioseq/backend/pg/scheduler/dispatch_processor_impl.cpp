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
#include <iterator>
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

module bioseq.scheduler.dispatch_processor;

import bioseq.common.mlogging;
import bioseq.common.hashing;
import bioseq.common.sequan_compat;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_index_run_buffer_bytes, bioseq_scheduler_*, bioseq_index_memory_cap_mb GUCs
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
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

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w1(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w2(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w4(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w8(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w1(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w2(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w4(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w8(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>>
dispatch_process_sequences_typed(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx)
{
    static_assert(
        pgseq_supported_alphabets::alphabet_count == 2,
        "BatchProcessor typed dispatcher must be extended when pgseq_supported_alphabets changes");
    static constexpr uint8_t dna4_code =
        static_cast<uint8_t>(pgseq_supported_alphabets::index_of<seq::dna4>());
    static constexpr uint8_t dna5_code =
        static_cast<uint8_t>(pgseq_supported_alphabets::index_of<seq::dna5>());

    switch (alphabet_code) {
        case dna4_code:
            switch (storage_unit_byte_width) {
                case 1: return process_sequences_dna4_w1(
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
                case 2: return process_sequences_dna4_w2(
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
                case 4: return process_sequences_dna4_w4(
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
                case 8: return process_sequences_dna4_w8(
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
            break;
        case dna5_code:
            switch (storage_unit_byte_width) {
                case 1: return process_sequences_dna5_w1(
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
                case 2: return process_sequences_dna5_w2(
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
                case 4: return process_sequences_dna5_w4(
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
                case 8: return process_sequences_dna5_w8(
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
            break;
    }
    ASSERT(false, fmt::format(
        "BatchProcessor::process_sequences invalid typmod alphabet={} width={}",
        static_cast<unsigned>(alphabet_code),
        static_cast<unsigned>(storage_unit_byte_width)));
    return {};
}

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> BatchProcessor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<MatchResult>> results;
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;

    if (!per_row_query_mode && query_hdr == nullptr) {
        bioseq_log<DBG>("[bioseq] pg: BatchProcessor::process_sequences received null query_hdr; returning empty map");
        return results;
    }
    if (!per_row_query_mode
        && (query_hdr->alphabet_code != alphabet_code
            || query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
        bioseq_log<DBG>("[bioseq] pg: BatchProcessor::process_sequences query_hdr typmod ({},{}) disagrees with context ({},{}); refusing to dispatch",
                static_cast<unsigned>(query_hdr->alphabet_code),
                static_cast<unsigned>(query_hdr->storage_unit_byte_width),
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
        return results;
    }

    auto run_pipeline_body =
        [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
         per_row_query_hdrs, &target_hdrs, &query_ids, &base_params,
         invocation_role, broadcast_query_ordinal, &stats, scanner_pg_ctx,
         &results] {
        results = dispatch_process_sequences_typed(
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
    };

    if (base_params.enable_profiling) {
        /*
         * Profiling pipeline. The ProfilerBlock is the persistent
         * process-wide singleton (see global_state.cppm); aggregates
         * accumulate across every batch in the current scan. The
         * one-shot summary fires at scan end inside
         * BioseqColumnScan_EndCustomScan (Part 48.5/48.8 of the
         * design doc) — emitting it here would produce one summary
         * per batch and accumulate duplicates on multi-batch scans.
         */
        using Args = bioseq::instr::context_args_profiling_full;
        using TimingArgs = bioseq::instr::context_args_profiling_timing_only;
        auto& profiler_block =
            bioseq::instr::persistent_profiler_block<Args>();
        auto& timing_profiler_block =
            bioseq::instr::persistent_profiler_block<TimingArgs>();
        bioseq::instr::installed_profiler_scope<Args> guard(profiler_block);
        bioseq::instr::installed_profiler_scope<TimingArgs> timing_guard(timing_profiler_block);
        static const auto* batch_desc =
            bioseq::instr::register_region_static("BatchProcessor.process_sequences");
        auto _batch_scope = bioseq::instr::region<Args>(batch_desc);
        run_pipeline_body();
    } else {
        run_pipeline_body();
    }

    return results;
}

BatchProcessor::CpuPreparedIndexGroup*
BatchProcessor::CpuPreparedTarget::find_group(
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const std::int32_t max_occ,
		const MinimizerSketchBackend sketch_backend) noexcept {
	for (CpuPreparedIndexGroup& group : groups) {
		if (group.kmer_size == kmer_size
			&& group.window_size == window_size
			&& group.max_occ == max_occ
			&& group.sketch_backend == sketch_backend) {
			return &group;
		}
	}
	return nullptr;
}

Minimap2Params BatchProcessor::effective_params_for_pair(
		const Minimap2Params& base,
		const std::size_t min_bp) noexcept {
	const bool all_defaults = base.kmer_size == 15
		&& base.window_size == 10
		&& base.min_chain_score == 40
		&& base.min_seed_count == 3;
	if (!all_defaults) {
		return base;
	}
	Minimap2Params scaled = base;
	if (min_bp < 50) {
		scaled.kmer_size = 8;
		scaled.window_size = 3;
		scaled.min_chain_score = 10;
		scaled.min_seed_count = 1;
	} else if (min_bp < 200) {
		scaled.kmer_size = 10;
		scaled.window_size = 5;
		scaled.min_chain_score = 20;
		scaled.min_seed_count = 2;
	}
	return scaled;
}

namespace {

template <class Alphabet, std::size_t Width>
void append_cpu_prepared_query_sketch(
		const pgseq_t& header,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<MinimapSketchData>& destination) {
	using sequence_t = compact_sequence_stdc_instance<
		pgseq_t, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	sketch_collector collector;
	const auto proof = pipe::minimizer_dispatch_range<MinimapSketchData>(
		collector,
		query,
		0,
		static_cast<bioseq::storage::sequence_size_t>(query.size()),
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend);
	if (!pipe::minimizer_range_proof_matches_backend(
			sketch_backend,
			proof,
			/*require_nonempty_vector_work=*/false)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin prepared query sketch proof does not match the selected backend")));
	}
	auto sketches = std::move(collector).extract_sketches_data();
	destination.insert(
		destination.end(),
		std::make_move_iterator(sketches.begin()),
		std::make_move_iterator(sketches.end()));
}

void dispatch_append_cpu_prepared_query_sketch(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const pgseq_t& header,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<MinimapSketchData>& destination) {
	static_assert(pgseq_supported_alphabets::alphabet_count == 2);
	constexpr std::uint8_t dna4_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna4>());
	constexpr std::uint8_t dna5_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna5>());
#define BIOSEQ_APPEND_PREPARED(ALPHABET, WIDTH) \
	append_cpu_prepared_query_sketch<ALPHABET, WIDTH>( \
		header, kmer_size, window_size, sketch_backend, destination)
	if (alphabet_code == dna4_code) {
		switch (storage_unit_byte_width) {
			case 1: return BIOSEQ_APPEND_PREPARED(seq::dna4, 1);
			case 2: return BIOSEQ_APPEND_PREPARED(seq::dna4, 2);
			case 4: return BIOSEQ_APPEND_PREPARED(seq::dna4, 4);
			case 8: return BIOSEQ_APPEND_PREPARED(seq::dna4, 8);
			default: break;
		}
	} else if (alphabet_code == dna5_code) {
		switch (storage_unit_byte_width) {
			case 1: return BIOSEQ_APPEND_PREPARED(seq::dna5, 1);
			case 2: return BIOSEQ_APPEND_PREPARED(seq::dna5, 2);
			case 4: return BIOSEQ_APPEND_PREPARED(seq::dna5, 4);
			case 8: return BIOSEQ_APPEND_PREPARED(seq::dna5, 8);
			default: break;
		}
	}
#undef BIOSEQ_APPEND_PREPARED
	ereport(ERROR,
		(errcode(ERRCODE_DATATYPE_MISMATCH),
		 errmsg("BioseqBatchJoin CPU preparation received an unsupported typmod")));
}

} // namespace

void BatchProcessor::prepare_cpu_shared_query_unions(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const std::uint64_t target_bp_size,
		const std::span<const pgseq_t* const> query_headers,
		const Minimap2Params& base_params,
		const bool include_persisted_parameter_group,
		CpuPreparedTarget& prepared_target) {
	prepared_target.groups.clear();
	prepared_target.target_bp_size = target_bp_size;
	prepared_target.query_count = query_headers.size();
	prepared_target.physical_target_index_reads = 0;
	prepared_target.total_unique_keys = 0;
	prepared_target.key_set_checksum = 1469598103934665603ull;
	prepared_target.query_union_admission_bytes = 0;
	prepared_target.query_union_permit.reset();
	prepared_target.unions_ready = false;
	if (base_params.vulkan_enabled) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Vulkan BatchJoin cannot use CPU target preparation")));
	}
	for (const pgseq_t* query_header : query_headers) {
		if (query_header == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("BioseqBatchJoin CPU preparation received a NULL query header")));
		}
		prepared_target.query_union_admission_bytes =
			bioseq::parallelism::saturating_add(
				prepared_target.query_union_admission_bytes,
				bioseq::parallelism::saturating_mul(
					static_cast<std::uint64_t>(query_header->bp_size),
					static_cast<std::uint64_t>(sizeof(MinimapSketchData))));
	}
	prepared_target.query_union_admission_bytes =
		bioseq::parallelism::saturating_add(
			prepared_target.query_union_admission_bytes,
			bioseq::parallelism::saturating_mul(
				static_cast<std::uint64_t>(query_headers.size()),
				static_cast<std::uint64_t>(sizeof(CpuPreparedIndexGroup))));
	if (prepared_target.query_union_admission_bytes
		== (std::numeric_limits<std::uint64_t>::max)()) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("BioseqBatchJoin query-union admission overflow")));
	}
	auto union_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			prepared_target.query_union_admission_bytes);
	if (!union_permit) {
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("BioseqBatchJoin query-hash union cannot be admitted"),
			 errdetail("required_bytes=%llu",
				static_cast<unsigned long long>(
					prepared_target.query_union_admission_bytes))));
	}
	prepared_target.query_union_permit.emplace(std::move(*union_permit));
	auto ensure_group = [&prepared_target](
			const std::int32_t k,
			const std::int32_t w,
			const std::int32_t max_occ,
			const MinimizerSketchBackend sketch_backend)
			-> CpuPreparedIndexGroup& {
		if (auto* existing = prepared_target.find_group(
				k, w, max_occ, sketch_backend)) {
			return *existing;
		}
		prepared_target.groups.push_back(CpuPreparedIndexGroup{
			.kmer_size = k,
			.window_size = w,
			.max_occ = max_occ,
			.sketch_backend = sketch_backend,
		});
		return prepared_target.groups.back();
	};
	for (const pgseq_t* query_header : query_headers) {
		if (query_header == nullptr
			|| query_header->alphabet_code != alphabet_code
			|| query_header->storage_unit_byte_width
				!= storage_unit_byte_width) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin query typmod changed during CPU preparation")));
		}
		const Minimap2Params effective = effective_params_for_pair(
			base_params,
			std::min<std::uint64_t>(target_bp_size, query_header->bp_size));
		const MinimizerSketchBackend sketch_backend =
			pipe::target_index_sketch_backend_from_params(effective);
		auto append_to = [&](CpuPreparedIndexGroup& group) {
			dispatch_append_cpu_prepared_query_sketch(
				alphabet_code,
				storage_unit_byte_width,
				*query_header,
				group.kmer_size,
				group.window_size,
				group.sketch_backend,
				group.query_hash_union);
		};
		if (include_persisted_parameter_group) {
			append_to(ensure_group(
				15, 10, effective.max_occ, sketch_backend));
		} else {
			append_to(ensure_group(
				effective.kmer_size,
				effective.window_size,
				effective.max_occ,
				sketch_backend));
		}
	}
	for (CpuPreparedIndexGroup& group : prepared_target.groups) {
		std::sort(
			group.query_hash_union.begin(),
			group.query_hash_union.end(),
			[](const MinimapSketchData& lhs,
			   const MinimapSketchData& rhs) noexcept {
				return lhs.kmer() < rhs.kmer();
			});
		group.query_hash_union.erase(
			std::unique(
				group.query_hash_union.begin(),
				group.query_hash_union.end(),
				[](const MinimapSketchData& lhs,
				   const MinimapSketchData& rhs) noexcept {
					return lhs.kmer() == rhs.kmer();
				}),
			group.query_hash_union.end());
		group.key_set_checksum =
			bioseq::storage::indexing::query_minimizer_signature(
				group.query_hash_union,
				static_cast<std::uint32_t>(group.kmer_size),
				static_cast<std::uint32_t>(group.window_size),
				static_cast<std::uint32_t>(group.max_occ));
		prepared_target.total_unique_keys =
			bioseq::parallelism::saturating_add(
				prepared_target.total_unique_keys,
				static_cast<std::uint64_t>(group.query_hash_union.size()));
		auto mix = [&prepared_target](const std::uint64_t value) noexcept {
			prepared_target.key_set_checksum ^= value;
			prepared_target.key_set_checksum *= 1099511628211ull;
		};
		mix(static_cast<std::uint64_t>(group.kmer_size));
		mix(static_cast<std::uint64_t>(group.window_size));
		mix(static_cast<std::uint64_t>(group.max_occ));
		mix(static_cast<std::uint64_t>(group.sketch_backend));
		mix(static_cast<std::uint64_t>(group.query_hash_union.size()));
		mix(group.key_set_checksum);
	}
	prepared_target.query_union_admission_bytes =
		bioseq::parallelism::saturating_add(
			bioseq::parallelism::saturating_mul(
				prepared_target.total_unique_keys,
				static_cast<std::uint64_t>(sizeof(MinimapSketchData))),
			static_cast<std::uint64_t>(
				prepared_target.groups.size()
				* sizeof(CpuPreparedIndexGroup)));
	prepared_target.query_union_permit->shrink_to(
		prepared_target.query_union_admission_bytes);
	prepared_target.unions_ready = true;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
