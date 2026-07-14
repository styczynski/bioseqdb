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
import bioseq.indexing.storage;
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
		std::vector<MinimapSketchData>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes) {
	using sequence_t = compact_sequence_stdc_instance<
		pgseq_t, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	const auto fail = [](const char* message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", message)));
		});
		pg_unreachable();
	};
	auto collector_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			0u);
	if (!collector_permit) {
		fail("BioseqBatchJoin query sketch collector admission owner is unavailable");
	}
	admitted_sketch_collector_t<
		MinimapSketchData,
		bioseq::parallelism::MemoryPermit> collector{
			std::move(*collector_permit),
			bioseq::parallelism::kVectorPreallocationHeadroomFactor};
	const auto proof = pipe::minimizer_dispatch_range<MinimapSketchData>(
		collector,
		query,
		0,
		static_cast<bioseq::storage::sequence_size_t>(query.size()),
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend,
		pipe::ExactSimdRangeRoute::require_continuation);
	if (!collector.ok()) {
		fail("BioseqBatchJoin query sketch collector growth cannot be admitted");
	}
	if (!pipe::minimizer_range_proof_matches_backend(
			sketch_backend,
			proof,
			/*require_nonempty_vector_work=*/false,
			pipe::ExactSimdRangeRoute::require_continuation)) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin prepared query sketch proof does not match the selected backend")));
		});
		pg_unreachable();
	}
	const auto& sketches = collector.extract_sketches_data();
	const std::uint64_t measured_collector_bytes =
		bioseq::parallelism::saturating_mul(
			sketches.capacity(), sizeof(MinimapSketchData));
	if (measured_collector_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_collector_bytes
			!= collector.admitted_capacity_bytes()) {
		fail("BioseqBatchJoin query sketch collector capacity disagrees with admission");
	}
	if (sketches.size() > destination.max_size() - destination.size()) {
		fail("BioseqBatchJoin query sketch destination size overflow");
	}
	const std::size_t required = destination.size() + sketches.size();
	if (required > destination.capacity()) {
		const std::uint64_t old_destination_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(), sizeof(MinimapSketchData));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				bioseq::parallelism::saturating_mul(
					required, sizeof(MinimapSketchData)),
				bioseq::parallelism::kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak = bioseq::parallelism::saturating_add(
			retained_destination_bytes, allocation_headroom);
		if (old_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_destination_bytes < old_destination_bytes
			|| !destination_permit.grow_to(peak)) {
			fail("BioseqBatchJoin query sketch destination growth cannot be admitted");
		}
		destination.reserve(required);
		const std::uint64_t measured_destination_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(), sizeof(MinimapSketchData));
		if (measured_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_destination_bytes > allocation_headroom) {
			fail("BioseqBatchJoin query sketch destination capacity exceeded admission");
		}
		retained_destination_bytes = bioseq::parallelism::saturating_add(
			retained_destination_bytes - old_destination_bytes,
			measured_destination_bytes);
		if (retained_destination_bytes
			== (std::numeric_limits<std::uint64_t>::max)()) {
			fail("BioseqBatchJoin retained query sketch capacity overflow");
		}
		destination_permit.shrink_to(retained_destination_bytes);
	}
	destination.insert(
		destination.end(),
		sketches.begin(), sketches.end());
}

void dispatch_append_cpu_prepared_query_sketch(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const pgseq_t& header,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<MinimapSketchData>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes) {
	static_assert(pgseq_supported_alphabets::alphabet_count == 2);
	constexpr std::uint8_t dna4_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna4>());
	constexpr std::uint8_t dna5_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna5>());
#define BIOSEQ_APPEND_PREPARED(ALPHABET, WIDTH) \
	append_cpu_prepared_query_sketch<ALPHABET, WIDTH>( \
		header, kmer_size, window_size, sketch_backend, destination, \
		destination_permit, retained_destination_bytes)
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
	bioseq::pg_utils::invoke_pg_as_cpp([] {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqBatchJoin CPU preparation received an unsupported typmod")));
	});
	pg_unreachable();
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
	prepared_target.release_query_union_state_noexcept();
	prepared_target.target_bp_size = target_bp_size;
	prepared_target.query_count = query_headers.size();
	prepared_target.physical_target_index_reads = 0;
	prepared_target.total_unique_keys = 0;
	prepared_target.key_set_checksum = 1469598103934665603ull;
	prepared_target.unions_ready = false;
	if (pipe::vulkan_components_requested(base_params)) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Vulkan BatchJoin cannot use CPU target preparation"),
			 errdetail("biological Vulkan components were requested for this statement")));
	}
	for (const pgseq_t* query_header : query_headers) {
		if (query_header == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("BioseqBatchJoin CPU preparation received a NULL query header")));
		}
	}
	auto union_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			0u);
	if (!union_permit) {
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("BioseqBatchJoin query-hash union admission owner is unavailable")));
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
		if (prepared_target.groups.size()
				== prepared_target.groups.capacity()) {
			const std::size_t required = prepared_target.groups.size() + 1u;
			const std::uint64_t old_outer_bytes =
				bioseq::parallelism::saturating_mul(
					prepared_target.groups.capacity(),
					sizeof(CpuPreparedIndexGroup));
			const std::uint64_t allocation_headroom =
				bioseq::parallelism::saturating_mul(
					bioseq::parallelism::saturating_mul(
						required, sizeof(CpuPreparedIndexGroup)),
					bioseq::parallelism::
						kVectorPreallocationHeadroomFactor);
			const std::uint64_t peak = bioseq::parallelism::saturating_add(
				prepared_target.query_union_admission_bytes,
				allocation_headroom);
			if (old_outer_bytes
					== (std::numeric_limits<std::uint64_t>::max)()
				|| allocation_headroom
					== (std::numeric_limits<std::uint64_t>::max)()
				|| peak == (std::numeric_limits<std::uint64_t>::max)()
				|| prepared_target.query_union_admission_bytes
					< old_outer_bytes
				|| !prepared_target.query_union_permit->grow_to(peak)) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin query-union group growth cannot be admitted")));
				});
				pg_unreachable();
			}
			prepared_target.groups.reserve(required);
			const std::uint64_t measured_outer_bytes =
				bioseq::parallelism::saturating_mul(
					prepared_target.groups.capacity(),
					sizeof(CpuPreparedIndexGroup));
			if (measured_outer_bytes
					== (std::numeric_limits<std::uint64_t>::max)()
				|| measured_outer_bytes > allocation_headroom) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin query-union group capacity exceeded admission")));
				});
				pg_unreachable();
			}
			prepared_target.query_union_admission_bytes =
				bioseq::parallelism::saturating_add(
					prepared_target.query_union_admission_bytes
						- old_outer_bytes,
					measured_outer_bytes);
			if (prepared_target.query_union_admission_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("BioseqBatchJoin retained group capacity overflow")));
				});
				pg_unreachable();
			}
			prepared_target.query_union_permit->shrink_to(
				prepared_target.query_union_admission_bytes);
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
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin query typmod changed during CPU preparation")));
			});
			pg_unreachable();
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
				group.query_hash_union,
				*prepared_target.query_union_permit,
				prepared_target.query_union_admission_bytes);
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
	const std::uint64_t tracked_query_union_bytes =
		prepared_target.query_union_admission_bytes;
	std::uint64_t measured_query_union_bytes =
		bioseq::parallelism::saturating_mul(
			prepared_target.groups.capacity(),
			static_cast<std::uint64_t>(sizeof(CpuPreparedIndexGroup)));
	for (const CpuPreparedIndexGroup& group : prepared_target.groups) {
		measured_query_union_bytes =
			bioseq::parallelism::saturating_add(
				measured_query_union_bytes,
				bioseq::parallelism::saturating_mul(
					group.query_hash_union.capacity(),
					static_cast<std::uint64_t>(
						sizeof(MinimapSketchData))));
	}
	if (measured_query_union_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_query_union_bytes != tracked_query_union_bytes
		|| measured_query_union_bytes
			> prepared_target.query_union_permit->bytes()) {
		const std::uint64_t retained =
			measured_query_union_bytes;
		const std::uint64_t admitted =
			prepared_target.query_union_permit->bytes();
		bioseq::pg_utils::invoke_pg_as_cpp([retained, admitted] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("BioseqBatchJoin retained query-union capacity exceeded admission"),
				 errdetail("retained_bytes=%llu admitted_headroom=%llu",
					static_cast<unsigned long long>(retained),
					static_cast<unsigned long long>(admitted))));
		});
		pg_unreachable();
	}
	prepared_target.query_union_admission_bytes = measured_query_union_bytes;
	prepared_target.query_union_permit->shrink_to(
		prepared_target.query_union_admission_bytes);
	prepared_target.unions_ready = true;
}

void BatchProcessor::prepare_cpu_persisted_grouped32_target(
		const Oid database_oid,
		const Oid tablespace_oid,
		const Oid main_relfilenode,
		const Oid user_relid,
		const std::int16_t column_attno,
		const std::string_view index_name,
		const std::uint64_t sequence_rowno,
		const bioseq::storage::indexing::BorrowedExecutorReadContextPg&
			executor_read,
		CpuPreparedTarget& prepared_target) {
	using namespace bioseq::storage::indexing;
	using bioseq::indexing::MinimizerGenerationWireError;
	using bioseq::indexing::MinimizerGenerationWireFormat;
	const auto fail = [](std::string message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([&] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("required compatible persisted minimizer index is invalid"),
				 errdetail("%s", message.c_str()),
				 errhint("Rebuild the exact revision-3 grouped32 READY generation; "
					"ordinal 2 has no legacy, recompute, or cache fallback.")));
		});
	};
	const auto unavailable = [](std::string message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([&] {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("required compatible persisted minimizer index unavailable"),
				 errdetail("%s", message.c_str()),
				 errhint("Build the exact revision-3 grouped32 READY generation; "
					"ordinal 2 has no legacy, recompute, or cache fallback.")));
		});
	};
	if (!prepared_target.unions_ready
		|| prepared_target.target_rowno < 0
		|| static_cast<std::uint64_t>(prepared_target.target_rowno)
			!= sequence_rowno
		|| prepared_target.groups.size() != 1
		|| database_oid != MyDatabaseId
		|| !OidIsValid(user_relid)
		|| !OidIsValid(tablespace_oid)
		|| !OidIsValid(main_relfilenode)
		|| column_attno <= 0
		|| !executor_read.matches(
			database_oid, tablespace_oid, main_relfilenode, user_relid)) {
		fail("CPU grouped32 prepared-target identity/lifecycle is invalid");
		return;
	}
	CpuPreparedIndexGroup& group = prepared_target.groups.front();
	if (group.kmer_size != 15 || group.window_size != 10
		|| group.max_occ < 0
		|| group.persisted_grouped32_reader
		|| group.target_index) {
		fail("CPU grouped32 parameter group is not the pinned cold-read shape");
		return;
	}

	Rev3IndexAdministrationIdentity identity{
		.database_oid = database_oid,
		.tablespace_oid = tablespace_oid,
		.main_relfilenode = main_relfilenode,
		.user_relid = user_relid,
		.column_attno = column_attno,
		.index_name = std::string{index_name},
	};
	auto source_proof = derive_rev3_catalog_source_proof_pg(
		identity, sequence_rowno, executor_read);
	const Rev3AuthoritativeSourceLocator source =
		source_proof.source_for_diagnostics();
	const Rev3CatalogSourceProofCounters source_counters =
		source_proof.counters_for_diagnostics();
	identity.table_name = format_seqidx_name(
		index_name, database_oid, tablespace_oid, main_relfilenode,
		column_attno, source.locator.bucket_idx, source.locator.shard_idx);
	const auto expected =
		bioseq::indexing::expected_minimizer_compatibility_for_backend(
			MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
			group.sketch_backend,
			source.alphabet_code,
			source.storage_unit_byte_width,
			static_cast<std::uint16_t>(group.kmer_size),
			static_cast<std::uint16_t>(group.window_size));
	if (!expected) {
		fail(fmt::format("grouped32 compatibility rejected wire_error={}",
			static_cast<unsigned>(expected.error())));
		return;
	}
	auto opened = open_exact_minimizer_generation_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read,
		*expected);
	if (!opened) {
		switch (opened.failure.kind) {
			case MinimizerGenerationRelationKind::missing:
				unavailable(fmt::format(
					"revision-3 generation relation {} is absent",
					identity.table_name));
				return;
			case MinimizerGenerationRelationKind::legacy_exact:
				unavailable(fmt::format(
					"relation {} is the exact legacy {} schema and has no "
					"revision-3 READY generation",
					identity.table_name, index_name));
				return;
			case MinimizerGenerationRelationKind::malformed:
				fail(fmt::format(
					"generation relation {} is neither the canonical revision-3 "
					"schema nor the exact legacy {} schema; revision3_schema_error={}",
					identity.table_name, index_name,
					rev3_index_schema_error_name(
						opened.failure.revision3_error)));
				return;
			case MinimizerGenerationRelationKind::revision3_exact:
				fail("generation relation binder returned an invalid success classification");
				return;
		}
	}
	IndexStorageFragmentGenerationReadSessionPg& session = *opened.session;
	const auto head = session.read_active_head_once();
	const auto& metadata = session.read_manifest_directory_once(head);
	const auto manifest = metadata.manifest;
	session.validate_source_attestation(std::move(source_proof));
	const std::uint64_t query_hash_bytes =
		bioseq::parallelism::saturating_mul(
			group.query_hash_union.size(), sizeof(std::uint64_t));
	if (query_hash_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
		fail("grouped32 query-hash conversion admission overflow");
		return;
	}
	const std::uint64_t query_hash_headroom =
		bioseq::parallelism::saturating_mul(
			query_hash_bytes,
			bioseq::parallelism::kVectorPreallocationHeadroomFactor);
	if (query_hash_headroom
		== (std::numeric_limits<std::uint64_t>::max)()) {
		fail("grouped32 query-hash conversion headroom overflow");
		return;
	}
	auto query_hash_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			query_hash_headroom);
	if (!query_hash_permit) {
		fail("grouped32 query-hash conversion was not admitted");
		return;
	}
	std::vector<std::uint64_t> query_hashes;
	query_hashes.reserve(group.query_hash_union.size());
	const std::uint64_t actual_query_hash_bytes =
		bioseq::parallelism::saturating_mul(
			query_hashes.capacity(), sizeof(std::uint64_t));
	if (actual_query_hash_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| actual_query_hash_bytes > query_hash_permit->bytes()) {
		fail("grouped32 query-hash conversion capacity exceeded admission");
		return;
	}
	query_hash_permit->shrink_to(actual_query_hash_bytes);
	for (const MinimapSketchData& minimizer : group.query_hash_union) {
		query_hashes.push_back(minimizer.kmer());
	}
	auto reader =
		bioseq::indexing::PreparedGrouped32QueryReader::
			begin_full_generation_stream(
				bioseq::parallelism::MemoryAdmissionController::instance(),
				head, manifest, *expected, group.sketch_backend,
				metadata.directory.encoded_bytes(), query_hashes,
				static_cast<std::uint64_t>(group.max_occ));
	if (!reader) {
		fail(fmt::format("grouped32 reader begin rejected wire_error={}",
			static_cast<unsigned>(reader.error())));
		return;
	}
	const auto accept_or_throw = [](auto result, std::string_view phase) {
		if (!result) {
			throw bioseq::indexing::PersistedMinimizerLookupError(
				fmt::format("{} wire_error={}", phase,
					static_cast<unsigned>(result.error())));
		}
	};
	(void)session.read_full_generation_payload_once(
		[&](const bioseq::indexing::IndexChunkAddress& address,
			const char* bytes, std::size_t byte_count) {
			accept_or_throw(
				reader->accept_full_generation_control_chunk(
					address, std::as_bytes(
						std::span<const char>{bytes, byte_count})),
				"grouped32 control ingest");
		},
		[&](const bioseq::indexing::IndexChunkAddress& address,
			const char* bytes, std::size_t byte_count) {
			accept_or_throw(
				reader->accept_full_generation_position_chunk(
					address, std::as_bytes(
						std::span<const char>{bytes, byte_count})),
				"grouped32 position ingest");
		});
	accept_or_throw(
		reader->finish_full_generation_stream(),
		"grouped32 final validation");
	const MinimizerGenerationReadStats read_stats = session.finish();
	const auto scheme =
		bioseq::indexing::minimizer_scheme_identity_for_backend(
			group.sketch_backend);
	const auto producer_backend =
		bioseq::indexing::minimizer_producer_backend_identity_for_backend(
			group.sketch_backend);
	if (!reader->ready()
		|| reader->qualified_sketch_backend() != group.sketch_backend
		|| reader->target_length() != prepared_target.target_bp_size
		|| manifest.kmer_size != group.kmer_size
		|| manifest.window_size != group.window_size
		|| manifest.minimizer_scheme_id != scheme.id
		|| manifest.minimizer_scheme_version != scheme.version
		|| manifest.producer_backend_id != producer_backend.id
		|| manifest.producer_backend_identity_version
			!= producer_backend.version
		|| head.generation_id == 0 || head.source_identity == 0
		|| read_stats.relation_opens != 1
		|| read_stats.index_scans != 3
		|| read_stats.head_scans != 1
		|| read_stats.manifest_directory_scans != 1
		|| read_stats.full_generation.scan_calls != 1
		|| read_stats.source_parent_rows_read != 1
		|| read_stats.source_catalog_rows_read != 1
		|| read_stats.source_catalog_index_scans != 1
		|| read_stats.source_fragment_rows_read != 0
		|| read_stats.source_decoded_bp != 0
		|| !source_counters.timed_equation_holds()) {
		fail("grouped32 READY identity/backend/scheme/read-work proof disagrees");
		return;
	}
	group.persisted_grouped32_reader.emplace(std::move(*reader));
	group.target_index_source =
		::bioseq::pipeline_stats::TargetIndexSource::persisted;
	group.persisted_generation_id = head.generation_id;
	group.persisted_source_identity = head.source_identity;
	group.persisted_read_stats = read_stats;
	group.persisted_reader_loads = 1;
	group.build_set_checksum_present = true;
	group.build_set_checksum = group.key_set_checksum;
	++prepared_target.physical_target_index_reads;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
