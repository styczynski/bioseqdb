/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor_impl.cppm
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

#include "common.h"
#include "headers/pg_build_config.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

module bioseq.scheduler.dispatch_processor;

import bioseq.common.mlogging;
import bioseq.common.hashing;
import bioseq.common.sequan_compat;
import bioseq.highway.dispatch_contract;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq::settings::index_run_buffer_bytes, bioseq_scheduler_*, bioseq::settings::index_memory_cap_mb GUCs
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

namespace bioseq::simd {

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w1(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
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
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
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
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);
#endif

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna4_w8(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w1(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
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
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
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
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);
#endif

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences_dna5_w8(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx);
#endif

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>>
dispatch_process_sequences_typed(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchInvocationRole invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
    BatchProcessor::ScannerPgContext* scanner_pg_ctx)
{
    static_assert(
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
        pgseq_supported_alphabets::alphabet_count == 1,
        "Level 0 BatchProcessor dispatch requires the dna4-only alphabet pack");
#else
        pgseq_supported_alphabets::alphabet_count == 2,
        "BatchProcessor typed dispatcher must be extended when pgseq_supported_alphabets changes");
#endif
    static constexpr uint8_t dna4_code =
        static_cast<uint8_t>(pgseq_supported_alphabets::index_of<seq::dna4>());
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
    static constexpr uint8_t dna5_code =
        static_cast<uint8_t>(pgseq_supported_alphabets::index_of<seq::dna5>());
#endif

    switch (alphabet_code) {
        case dna4_code:
            switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
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
#endif
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
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
        case dna5_code:
            switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
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
#endif
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
#endif
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
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
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

    const auto highway_plan =
        ::bioseq::simd_dispatch::registered_highway_dispatch_info();
    const auto& highway_binding =
        ::bioseq::simd_dispatch::select_highway_target_once(highway_plan);
    if (highway_binding.target == 0
        || highway_binding.kernels == nullptr
        || highway_binding.target != highway_plan.planned_target) {
        throw std::logic_error{
            "Highway runtime binding disagrees with the generated target plan"};
    }
    Minimap2Params dispatched_params = base_params;
    dispatched_params.highway_kernels = highway_binding.kernels;
    dispatched_params.highway_dispatch = highway_plan;
    dispatched_params.highway_executed_target = highway_binding.target;
    dispatched_params.alphabet_code = alphabet_code;

    auto run_pipeline_body =
        [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
         per_row_query_hdrs, &target_hdrs, &query_ids, &dispatched_params,
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
            dispatched_params,
            invocation_role,
            broadcast_query_ordinal,
            stats,
            scanner_pg_ctx);
    };

    const auto inherited_observer =
        bioseq::parallelism::current_tracked_job_observer();
    const auto requested_observer = scanner_pg_ctx != nullptr
            && scanner_pg_ctx->async_job_observer
        ? scanner_pg_ctx->async_job_observer
        : inherited_observer;
    const auto previous_observer =
        bioseq::parallelism::exchange_tracked_job_observer(
            requested_observer);
    try {
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
            using TimingArgs =
                bioseq::instr::context_args_profiling_timing_only;
            auto& profiler_block =
                bioseq::instr::persistent_profiler_block<Args>();
            auto& timing_profiler_block =
                bioseq::instr::persistent_profiler_block<TimingArgs>();
            bioseq::instr::installed_profiler_scope<Args> guard(
                profiler_block);
            bioseq::instr::installed_profiler_scope<TimingArgs> timing_guard(
                timing_profiler_block);
            static const auto* batch_desc =
                bioseq::instr::register_region_static(
                    "BatchProcessor.process_sequences");
            auto _batch_scope = bioseq::instr::region<Args>(batch_desc);
            run_pipeline_body();
        } else {
            run_pipeline_body();
        }
    } catch (...) {
        bioseq::parallelism::restore_tracked_job_observer(
            previous_observer);
        throw;
    }
    bioseq::parallelism::restore_tracked_job_observer(
        previous_observer);

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

/*
 * Format-6 preparation fans one sketch pass into the authenticated canonical
 * u32 union and, when separately admitted, the full per-query minimizer vector
 * later consumed by alignment.  This removes the typed row path's second
 * sketch without introducing a Q-sized conversion owner.
 *
 * The collector deliberately exposes the same small surface used by the
 * scalar and ExactSimd kernels.  Capacity remains charged to the prepared
 * target's aggregate permits, including vector reallocation overlap.  Retained
 * per-query growth is best-effort; canonical collection remains fail-closed.
 */
class AdmittedCanonicalU32QueryCollector {
public:
	using minimizer_type = MinimapSketchData;

	AdmittedCanonicalU32QueryCollector(
			std::vector<std::uint32_t>& destination,
			bioseq::parallelism::MemoryPermit& permit,
			std::uint64_t& retained_bytes,
			std::vector<MinimapSketchData>* retained_query = nullptr,
			bioseq::parallelism::MemoryPermit*
				retained_query_permit = nullptr,
			std::uint64_t* retained_query_bytes = nullptr) noexcept
		: destination_(destination),
		  permit_(permit),
		  retained_bytes_(retained_bytes),
		  retained_query_(retained_query),
		  retained_query_permit_(retained_query_permit),
		  retained_query_bytes_(retained_query_bytes),
		  retention_complete_(
			retained_query != nullptr
			&& retained_query_permit != nullptr
			&& retained_query_bytes != nullptr) {}

	[[nodiscard]] std::size_t size() const noexcept {
		return destination_.size();
	}

	[[nodiscard]] bool reserve(const std::size_t required) {
		return ensure_capacity(required);
	}

	[[always_inline]] void collect(const MinimapSketchData& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(minimizer);
	}

	[[always_inline]] void collect(MinimapSketchData&& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(std::move(minimizer));
	}

	template<class M = MinimapSketchData>
	[[nodiscard, always_inline]] bool append_raw_storage(
			const typename M::storage_t* raw,
			const std::size_t count)
		requires (
			std::same_as<M, MinimapSketchData>
			&& requires { typename M::storage_t; }
			&& std::is_trivially_copyable_v<M>
			&& sizeof(M) == sizeof(typename M::storage_t))
	{
		const std::size_t old_size = destination_.size();
		if (count > destination_.max_size() - old_size
			|| !ensure_capacity(old_size + count)) {
			failed_ = true;
			return false;
		}
		destination_.resize(old_size + count);
		for (std::size_t index = 0; index < count; ++index) {
			const std::uint64_t hash =
				MinimapSketchData::from_raw_storage(raw[index]).kmer();
			hash_width_valid_ =
				hash_width_valid_
				&& hash
					<= (std::numeric_limits<std::uint32_t>::max)();
			destination_[old_size + index] =
				static_cast<std::uint32_t>(hash);
		}
		append_retained_raw(raw, count);
		return true;
	}

	[[nodiscard]] bool ok() const noexcept {
		return !failed_;
	}

	[[nodiscard]] bool hash_width_valid() const noexcept {
		return hash_width_valid_;
	}

	[[nodiscard]] bool retention_complete() const noexcept {
		return retention_complete_;
	}

private:
	[[always_inline]] void append_hash(const std::uint64_t hash) {
		hash_width_valid_ =
			hash_width_valid_
			&& hash <= (std::numeric_limits<std::uint32_t>::max)();
		if (destination_.size() == destination_.max_size()
			|| !ensure_capacity(destination_.size() + 1u)) {
			failed_ = true;
			return;
		}
		destination_.push_back(static_cast<std::uint32_t>(hash));
	}

	void abandon_retention() noexcept {
		if (!retention_complete_
			|| retained_query_ == nullptr
			|| retained_query_permit_ == nullptr
			|| retained_query_bytes_ == nullptr) {
			retention_complete_ = false;
			return;
		}
		const std::uint64_t released_bytes =
			bioseq::parallelism::saturating_mul(
				retained_query_->capacity(),
				sizeof(MinimapSketchData));
		{
			std::vector<MinimapSketchData> empty;
			retained_query_->swap(empty);
		}
		if (released_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| *retained_query_bytes_ < released_bytes) {
			/* The caller's final measured-capacity reconciliation remains the
			 * correctness authority.  Retain the current permit reservation
			 * on impossible accounting instead of releasing unowned bytes. */
			retention_complete_ = false;
			return;
		}
		*retained_query_bytes_ -= released_bytes;
		retained_query_permit_->shrink_to(*retained_query_bytes_);
		retention_complete_ = false;
	}

	[[nodiscard]] bool ensure_retained_capacity(
			const std::size_t required) {
		if (!retention_complete_) return false;
		if (required <= retained_query_->capacity()) return true;
		if (required > retained_query_->max_size()) {
			abandon_retention();
			return false;
		}

		const std::size_t old_capacity = retained_query_->capacity();
		std::size_t requested_capacity = 8u;
		if (old_capacity != 0u) {
			requested_capacity =
				old_capacity <= retained_query_->max_size() / 2u
				? old_capacity * 2u
				: required;
		}
		requested_capacity = std::max(requested_capacity, required);
		const std::uint64_t old_bytes =
			bioseq::parallelism::saturating_mul(
				old_capacity, sizeof(MinimapSketchData));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				requested_capacity, sizeof(MinimapSketchData));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
				*retained_query_bytes_, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| *retained_query_bytes_ < old_bytes
			|| !retained_query_permit_->grow_to(peak)) {
			abandon_retention();
			return false;
		}
		try {
			retained_query_->reserve(requested_capacity);
		} catch (const std::bad_alloc&) {
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			abandon_retention();
			return false;
		} catch (...) {
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::parallelism::saturating_mul(
				retained_query_->capacity(),
				sizeof(MinimapSketchData));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			{
				std::vector<MinimapSketchData> empty;
				retained_query_->swap(empty);
			}
			*retained_query_bytes_ -= old_bytes;
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			retention_complete_ = false;
			return false;
		}
		const std::uint64_t reconciled_bytes =
			bioseq::parallelism::saturating_add(
				*retained_query_bytes_ - old_bytes,
				measured_bytes);
		if (reconciled_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			{
				std::vector<MinimapSketchData> empty;
				retained_query_->swap(empty);
			}
			*retained_query_bytes_ -= old_bytes;
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			retention_complete_ = false;
			return false;
		}
		*retained_query_bytes_ = reconciled_bytes;
		retained_query_permit_->shrink_to(*retained_query_bytes_);
		return true;
	}

	template<class Value>
	[[always_inline]] void append_retained(Value&& minimizer) {
		if (!retention_complete_) return;
		if (retained_query_->size() == retained_query_->max_size()
			|| !ensure_retained_capacity(retained_query_->size() + 1u)) {
			return;
		}
		retained_query_->push_back(std::forward<Value>(minimizer));
	}

	template<class Storage>
	[[always_inline]] void append_retained_raw(
			const Storage* raw,
			const std::size_t count) {
		if (!retention_complete_) return;
		const std::size_t old_size = retained_query_->size();
		if (count > retained_query_->max_size() - old_size
			|| !ensure_retained_capacity(old_size + count)) {
			return;
		}
		retained_query_->resize(old_size + count);
		if (count != 0u) {
			std::memcpy(
				static_cast<void*>(retained_query_->data() + old_size),
				static_cast<const void*>(raw),
				count * sizeof(Storage));
		}
	}

	[[nodiscard]] bool ensure_capacity(const std::size_t required) {
		if (failed_) return false;
		if (required <= destination_.capacity()) return true;
		if (required > destination_.max_size()) {
			failed_ = true;
			return false;
		}

		const std::size_t old_capacity = destination_.capacity();
		std::size_t requested_capacity = 8u;
		if (old_capacity != 0u) {
			requested_capacity =
				old_capacity <= destination_.max_size() / 2u
				? old_capacity * 2u
				: required;
		}
		requested_capacity = std::max(requested_capacity, required);

		const std::uint64_t old_bytes =
			bioseq::parallelism::saturating_mul(
				old_capacity, sizeof(std::uint32_t));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				requested_capacity, sizeof(std::uint32_t));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
				retained_bytes_, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_bytes_ < old_bytes
			|| !permit_.grow_to(peak)) {
			failed_ = true;
			return false;
		}

		try {
			destination_.reserve(requested_capacity);
		} catch (...) {
			permit_.shrink_to(retained_bytes_);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::parallelism::saturating_mul(
				destination_.capacity(), sizeof(std::uint32_t));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			failed_ = true;
			return false;
		}
		retained_bytes_ =
			bioseq::parallelism::saturating_add(
				retained_bytes_ - old_bytes, measured_bytes);
		if (retained_bytes_
				== (std::numeric_limits<std::uint64_t>::max)()) {
			failed_ = true;
			return false;
		}
		permit_.shrink_to(retained_bytes_);
		return true;
	}

	std::vector<std::uint32_t>& destination_;
	bioseq::parallelism::MemoryPermit& permit_;
	std::uint64_t& retained_bytes_;
	std::vector<MinimapSketchData>* retained_query_ = nullptr;
	bioseq::parallelism::MemoryPermit*
		retained_query_permit_ = nullptr;
	std::uint64_t* retained_query_bytes_ = nullptr;
	bool failed_ = false;
	bool hash_width_valid_ = true;
	bool retention_complete_ = false;
};

void release_uncommitted_retained_query(
		std::vector<MinimapSketchData>& minimizers,
		bioseq::parallelism::MemoryPermit& permit,
		std::uint64_t& retained_bytes) noexcept {
	const std::uint64_t released_bytes =
		bioseq::parallelism::saturating_mul(
			minimizers.capacity(), sizeof(MinimapSketchData));
	{
		std::vector<MinimapSketchData> empty;
		minimizers.swap(empty);
	}
	if (released_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| retained_bytes < released_bytes) {
		return;
	}
	retained_bytes -= released_bytes;
	permit.shrink_to(retained_bytes);
}

[[nodiscard]] bool try_commit_retained_query_sketch(
		const std::uint64_t query_ordinal,
		const std::uint64_t query_bp_size,
		std::vector<MinimapSketchData>&& minimizers,
		pipe::MinimizerRangeExecutionProof proof,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>& retained_queries,
		bioseq::parallelism::MemoryPermit& permit,
		std::uint64_t& retained_bytes) {
	if (!retained_queries.empty()
		&& retained_queries.back().query_ordinal >= query_ordinal) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin retained query ordinals are not strictly ordered")));
		});
		pg_unreachable();
	}
	if (retained_queries.size() == retained_queries.max_size()) {
		release_uncommitted_retained_query(
			minimizers, permit, retained_bytes);
		return false;
	}
	if (retained_queries.size() == retained_queries.capacity()) {
		const std::size_t required = retained_queries.size() + 1u;
		const std::uint64_t old_bytes =
			bioseq::parallelism::saturating_mul(
				retained_queries.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				required,
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
				retained_bytes, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_bytes < old_bytes
			|| !permit.grow_to(peak)) {
			release_uncommitted_retained_query(
				minimizers, permit, retained_bytes);
			return false;
		}
		try {
			retained_queries.reserve(required);
		} catch (const std::bad_alloc&) {
			permit.shrink_to(retained_bytes);
			release_uncommitted_retained_query(
				minimizers, permit, retained_bytes);
			return false;
		} catch (...) {
			permit.shrink_to(retained_bytes);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::parallelism::saturating_mul(
				retained_queries.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("BioseqBatchJoin retained query cache capacity exceeded admission")));
			});
			pg_unreachable();
		}
		retained_bytes =
			bioseq::parallelism::saturating_add(
				retained_bytes - old_bytes, measured_bytes);
		if (retained_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("BioseqBatchJoin retained query cache capacity overflow")));
			});
			pg_unreachable();
		}
		permit.shrink_to(retained_bytes);
	}
	retained_queries.push_back(BatchProcessor::CpuPreparedQuerySketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = query_bp_size,
		.minimizers = std::move(minimizers),
		.proof = std::move(proof),
	});
	return true;
}

template <class Alphabet, class DestinationValue>
void append_cpu_prepared_query_sketch_view(
		const bioseq::storage::basic_lazy_sequence_view<Alphabet> query_view,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>*
			retained_queries,
		bioseq::parallelism::MemoryPermit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	static_assert(
		std::is_same_v<DestinationValue, MinimapSketchData>
		|| std::is_same_v<DestinationValue, std::uint32_t>);
	if (!minimizer_query_length_is_representable<MinimapSketchData>(
			query_view.bp_size())) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BioseqBatchJoin query exceeds the 28-bit minimizer position domain")));
		});
		pg_unreachable();
	}
	const auto fail = [](const char* message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", message)));
		});
		pg_unreachable();
	};
	if constexpr (std::is_same_v<DestinationValue, std::uint32_t>) {
		std::vector<MinimapSketchData> retained_query;
		AdmittedCanonicalU32QueryCollector collector{
			destination,
			destination_permit,
			retained_destination_bytes,
			retained_queries != nullptr
					&& retained_query_permit != nullptr
					&& retained_query_bytes != nullptr
				? &retained_query
				: nullptr,
			retained_query_permit,
			retained_query_bytes};
		auto proof = pipe::minimizer_dispatch_indexed_query_whole<
			MinimapSketchData>(
			collector,
			query_view,
			static_cast<std::size_t>(window_size),
			static_cast<std::size_t>(kmer_size),
			sketch_backend);
		if (!collector.hash_width_valid()) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Format-6 canonical query hash exceeds its authenticated u32 profile")));
			});
			pg_unreachable();
		}
		if (!collector.ok()) {
			fail("BioseqBatchJoin canonical query-hash destination growth cannot be admitted");
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
		if (collector.retention_complete()) {
			(void)try_commit_retained_query_sketch(
				query_ordinal,
				static_cast<std::uint64_t>(query_view.bp_size()),
				std::move(retained_query),
				std::move(proof),
				*retained_queries,
				*retained_query_permit,
				*retained_query_bytes);
		}
		return;
	} else {
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
	const auto proof = pipe::minimizer_dispatch_indexed_query_whole<
		MinimapSketchData>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend);
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
				destination.capacity(), sizeof(DestinationValue));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				bioseq::parallelism::saturating_mul(
					required, sizeof(DestinationValue)),
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
				destination.capacity(), sizeof(DestinationValue));
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
}

template <class Alphabet, std::size_t Width, class DestinationValue>
void append_cpu_prepared_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>* retained_queries,
		bioseq::parallelism::MemoryPermit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	append_cpu_prepared_query_sketch_view<Alphabet, DestinationValue>(
		bioseq::storage::as_lazy_sequence_view(query),
		query_ordinal, kmer_size, window_size, sketch_backend,
		destination, destination_permit, retained_destination_bytes,
		retained_queries, retained_query_permit, retained_query_bytes);
}

template <class DestinationValue>
void dispatch_append_cpu_prepared_query_sketch(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>*
			retained_queries = nullptr,
		bioseq::parallelism::MemoryPermit* retained_query_permit = nullptr,
		std::uint64_t* retained_query_bytes = nullptr) {
	static_assert(
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
		pgseq_supported_alphabets::alphabet_count == 1
#else
		pgseq_supported_alphabets::alphabet_count == 2
#endif
	);
	constexpr std::uint8_t dna4_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna4>());
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
	constexpr std::uint8_t dna5_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna5>());
#endif
	#define BIOSEQ_APPEND_PREPARED(ALPHABET, WIDTH) \
		append_cpu_prepared_query_sketch<ALPHABET, WIDTH, DestinationValue>( \
			header, query_ordinal, kmer_size, window_size, sketch_backend, \
			destination, destination_permit, retained_destination_bytes, \
			retained_queries, retained_query_permit, retained_query_bytes)
	if (alphabet_code == dna4_code) {
		switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
			case 1: return BIOSEQ_APPEND_PREPARED(seq::dna4, 1);
			case 2: return BIOSEQ_APPEND_PREPARED(seq::dna4, 2);
			case 4: return BIOSEQ_APPEND_PREPARED(seq::dna4, 4);
#endif
			case 8: return BIOSEQ_APPEND_PREPARED(seq::dna4, 8);
			default: break;
		}
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
	} else if (alphabet_code == dna5_code) {
		switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
			case 1: return BIOSEQ_APPEND_PREPARED(seq::dna5, 1);
			case 2: return BIOSEQ_APPEND_PREPARED(seq::dna5, 2);
			case 4: return BIOSEQ_APPEND_PREPARED(seq::dna5, 4);
#endif
			case 8: return BIOSEQ_APPEND_PREPARED(seq::dna5, 8);
			default: break;
		}
#endif
	}
#undef BIOSEQ_APPEND_PREPARED
	bioseq::pg_utils::invoke_pg_as_cpp([] {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqBatchJoin CPU preparation received an unsupported typmod")));
	});
	pg_unreachable();
}

template <class DestinationValue>
void dispatch_append_cpu_prepared_query_recipe_sketch(
		const std::uint8_t alphabet_code,
		const Datum recipe,
		const pg_sequence_descriptor& descriptor,
		pg_sequence_read_session& session,
		const bioseq::storage::sequence_payload_allocation_observer*
			payload_observer,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>*
			retained_queries = nullptr,
		bioseq::parallelism::MemoryPermit* retained_query_permit = nullptr,
		std::uint64_t* retained_query_bytes = nullptr) {
	const auto append = [&session, recipe, descriptor, payload_observer,
		query_ordinal, kmer_size, window_size, sketch_backend,
		&destination, &destination_permit, &retained_destination_bytes,
		retained_queries, retained_query_permit,
		retained_query_bytes]<class Alphabet>() {
		auto* owner = bioseq::pg_utils::invoke_pg_as_cpp(
			[&session, recipe, descriptor] {
				return ::make_pg_sequence_call_owner<Alphabet>(
					session, recipe, descriptor);
			});
		append_cpu_prepared_query_sketch_view<Alphabet, DestinationValue>(
			owner->view().with_payload_allocation_observer(payload_observer),
			query_ordinal, kmer_size, window_size,
			sketch_backend, destination, destination_permit,
			retained_destination_bytes, retained_queries,
			retained_query_permit, retained_query_bytes);
	};
	constexpr std::uint8_t dna4_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna4>());
	if (alphabet_code == dna4_code) {
		append.template operator()<seq::dna4>();
		return;
	}
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
	constexpr std::uint8_t dna5_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna5>());
	if (alphabet_code == dna5_code) {
		append.template operator()<seq::dna5>();
		return;
	}
#endif
	bioseq::pg_utils::invoke_pg_as_cpp([] {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqBatchJoin recipe preparation received an unsupported alphabet")));
	});
	pg_unreachable();
}

enum class ParallelPreparedQuerySketchStatus : std::uint8_t {
	pending,
	ready,
	query_length_unsupported,
	admission_unavailable,
	collector_growth_failed,
	capacity_mismatch,
	proof_invalid,
	unsupported_typmod,
};

template <class Alphabet, std::size_t Width>
[[nodiscard]] ParallelPreparedQuerySketchStatus
prepare_independently_admitted_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		BatchProcessor::CpuPreparedQuerySketch& destination) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	auto query_owner = bioseq::storage::make_lazy_in_memory(query);
	const auto query_view =
		bioseq::storage::as_lazy_sequence_view(query_owner);
	if (!minimizer_query_length_is_representable<MinimapSketchData>(
			static_cast<bioseq::storage::sequence_size_t>(query.bp_size()))) {
		return ParallelPreparedQuerySketchStatus::query_length_unsupported;
	}
	auto permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			0u);
	if (!permit) {
		return ParallelPreparedQuerySketchStatus::admission_unavailable;
	}
	admitted_sketch_collector_t<
		MinimapSketchData,
		bioseq::parallelism::MemoryPermit> collector{
			std::move(*permit),
			bioseq::parallelism::kVectorPreallocationHeadroomFactor};
	auto proof = pipe::minimizer_dispatch_indexed_query_whole<
		MinimapSketchData>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend);
	if (!collector.ok()) {
		return ParallelPreparedQuerySketchStatus::collector_growth_failed;
	}
	if (!pipe::minimizer_range_proof_matches_backend(
			sketch_backend,
			proof,
			/*require_nonempty_vector_work=*/false,
			pipe::ExactSimdRangeRoute::require_continuation)) {
		return ParallelPreparedQuerySketchStatus::proof_invalid;
	}
	auto owned = std::move(collector).release_owned_storage();
	const std::uint64_t measured_bytes =
		bioseq::parallelism::saturating_mul(
			owned.first.capacity(), sizeof(MinimapSketchData));
	if (measured_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_bytes != owned.second.bytes()) {
		return ParallelPreparedQuerySketchStatus::capacity_mismatch;
	}
	destination = BatchProcessor::CpuPreparedQuerySketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = static_cast<std::uint64_t>(query.bp_size()),
		.parallel_preparation_permit =
			std::optional<bioseq::parallelism::MemoryPermit>{
				std::move(owned.second)},
		.minimizers = std::move(owned.first),
		.proof = std::move(proof),
	};
	return ParallelPreparedQuerySketchStatus::ready;
}

[[nodiscard]] ParallelPreparedQuerySketchStatus
dispatch_prepare_independently_admitted_query_sketch(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		BatchProcessor::CpuPreparedQuerySketch& destination) {
	constexpr std::uint8_t dna4_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna4>());
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
	constexpr std::uint8_t dna5_code = static_cast<std::uint8_t>(
		pgseq_supported_alphabets::index_of<seq::dna5>());
#endif
	#define BIOSEQ_PREPARE_INDEPENDENT(ALPHABET, WIDTH) \
		return prepare_independently_admitted_query_sketch<ALPHABET, WIDTH>( \
			header, query_ordinal, kmer_size, window_size, sketch_backend, \
			destination)
	if (alphabet_code == dna4_code) {
		switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
			case 1: BIOSEQ_PREPARE_INDEPENDENT(seq::dna4, 1);
			case 2: BIOSEQ_PREPARE_INDEPENDENT(seq::dna4, 2);
			case 4: BIOSEQ_PREPARE_INDEPENDENT(seq::dna4, 4);
#endif
			case 8: BIOSEQ_PREPARE_INDEPENDENT(seq::dna4, 8);
			default: break;
		}
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
	} else if (alphabet_code == dna5_code) {
		switch (storage_unit_byte_width) {
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 2
			case 1: BIOSEQ_PREPARE_INDEPENDENT(seq::dna5, 1);
			case 2: BIOSEQ_PREPARE_INDEPENDENT(seq::dna5, 2);
			case 4: BIOSEQ_PREPARE_INDEPENDENT(seq::dna5, 4);
#endif
			case 8: BIOSEQ_PREPARE_INDEPENDENT(seq::dna5, 8);
			default: break;
		}
#endif
	}
#undef BIOSEQ_PREPARE_INDEPENDENT
	return ParallelPreparedQuerySketchStatus::unsupported_typmod;
}

[[nodiscard]] bool reserve_parallel_query_sketch_slots(
		std::vector<BatchProcessor::CpuPreparedQuerySketch>& destination,
		const std::size_t required,
		bioseq::parallelism::MemoryPermit& permit,
		std::uint64_t& retained_bytes) {
	if (required > destination.max_size()) {
		return false;
	}
	if (required > destination.capacity()) {
		const std::uint64_t old_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				required,
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
				retained_bytes, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_bytes < old_bytes
			|| !permit.grow_to(peak)) {
			return false;
		}
		try {
			destination.reserve(required);
		} catch (const std::bad_alloc&) {
			permit.shrink_to(retained_bytes);
			return false;
		} catch (...) {
			permit.shrink_to(retained_bytes);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			return false;
		}
		retained_bytes =
			bioseq::parallelism::saturating_add(
				retained_bytes - old_bytes, measured_bytes);
		if (retained_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			return false;
		}
		permit.shrink_to(retained_bytes);
	}
	destination.resize(required);
	return true;
}

using Grouped32ColdPlan =
	bioseq::indexing::MinimizerGrouped32ColdRoutePlan;
using Grouped32PayloadGroup =
	bioseq::storage::indexing::MinimizerGenerationPayloadReadGroup;

[[nodiscard]] bool grouped32_requests_are_adjacent(
		const bioseq::indexing::IndexSubKeyRequest& previous,
		const bioseq::indexing::IndexSubKeyRequest& current) noexcept {
	return previous.generation_id == current.generation_id
		&& previous.position_segment == current.position_segment
		&& previous.sub_key
			!= (std::numeric_limits<bioseq::indexing::index_sub_key_t>::max)()
		&& previous.sub_key + 1 == current.sub_key;
}

[[nodiscard]] std::expected<
	std::vector<Grouped32PayloadGroup>,
	bioseq::indexing::MinimizerGenerationWireError>
group_grouped32_cold_requests(
		std::span<const bioseq::indexing::IndexSubKeyRequest> requests) {
	using bioseq::indexing::MinimizerGenerationWireError;
	std::vector<Grouped32PayloadGroup> groups;
	groups.reserve(requests.size());
	std::optional<bioseq::indexing::IndexSubKeyRequest> previous;
	for (const bioseq::indexing::IndexSubKeyRequest& request : requests) {
		if (request.generation_id == 0 || request.position_segment < 0
			|| request.sub_key < 0
			|| request.sub_key
				== (std::numeric_limits<
					bioseq::indexing::index_sub_key_t>::max)()
			|| request.expected_chunk_count == 0
			|| request.expected_total_bytes == 0) {
			return std::unexpected(MinimizerGenerationWireError::chunk_range);
		}
		if (previous) {
			if (previous->generation_id != request.generation_id) {
				return std::unexpected(
					MinimizerGenerationWireError::identity_mismatch);
			}
			const auto previous_key = std::pair{
				previous->position_segment, previous->sub_key};
			const auto current_key = std::pair{
				request.position_segment, request.sub_key};
			if (!(previous_key < current_key)) {
				return std::unexpected(
					MinimizerGenerationWireError::control_order);
			}
		}
		if (!previous
			|| !grouped32_requests_are_adjacent(*previous, request)) {
			groups.push_back(Grouped32PayloadGroup{
				.range = bioseq::indexing::IndexSubKeyRange{
					.generation_id = request.generation_id,
					.position_segment = request.position_segment,
					.begin = request.sub_key,
					.end = request.sub_key + 1,
				},
			});
		} else {
			groups.back().range.end = request.sub_key + 1;
		}
		groups.back().requests.push_back(request);
		previous = request;
	}
	return groups;
}

[[nodiscard]] bool grouped32_group_authority_equals(
		std::span<const Grouped32PayloadGroup> groups,
		std::span<const bioseq::indexing::IndexSubKeyRequest> authority)
		noexcept {
	std::size_t cursor = 0;
	for (const Grouped32PayloadGroup& group : groups) {
		for (const bioseq::indexing::IndexSubKeyRequest& request
			 : group.requests) {
			if (cursor >= authority.size()
				|| !bioseq::indexing::
					minimizer_grouped32_cold_request_equal(
						request, authority[cursor])) {
				return false;
			}
			++cursor;
		}
	}
	return cursor == authority.size();
}

[[nodiscard]] std::expected<
	std::vector<Grouped32PayloadGroup>,
	bioseq::indexing::MinimizerGenerationWireError>
group_retained_grouped32_position_requests(
		const Grouped32ColdPlan& plan,
		std::span<const std::uint32_t> retained_ordinals) {
	using bioseq::indexing::MinimizerGenerationWireError;
	std::vector<bioseq::indexing::IndexSubKeyRequest> retained_requests;
	retained_requests.reserve(retained_ordinals.size());
	std::size_t candidate_cursor = 0;
	std::optional<std::uint32_t> previous_retained;
	for (const std::uint32_t retained : retained_ordinals) {
		if (previous_retained && *previous_retained >= retained) {
			return std::unexpected(
				MinimizerGenerationWireError::partition_id);
		}
		while (candidate_cursor
				< plan.candidate_directory_ordinals.size()
			&& plan.candidate_directory_ordinals[candidate_cursor]
				< retained) {
			++candidate_cursor;
		}
		if (candidate_cursor
				== plan.candidate_directory_ordinals.size()
			|| plan.candidate_directory_ordinals[candidate_cursor]
				!= retained
			|| candidate_cursor
				>= plan.position_upper_bound_requests.size()) {
			return std::unexpected(
				MinimizerGenerationWireError::partition_id);
		}
		retained_requests.push_back(
			plan.position_upper_bound_requests[candidate_cursor]);
		++candidate_cursor;
		previous_retained = retained;
	}
	return group_grouped32_cold_requests(retained_requests);
}

} // namespace

void BatchProcessor::prepare_cpu_shared_query_unions(
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const std::uint64_t target_bp_size,
		const std::span<const bioseq::pg_types::seq* const> query_headers,
		const std::span<const Datum> query_recipe_datums,
		const std::span<const pg_sequence_descriptor>
			query_recipe_descriptors,
		pg_sequence_read_session* query_read_session,
		const bioseq::storage::sequence_payload_allocation_observer*
			query_payload_observer,
		const std::span<const std::uint64_t> query_ordinals,
		const Minimap2Params& base_params,
		const bool include_persisted_parameter_group,
		const CpuQueryUnionStorage union_storage,
		CpuPreparedTarget& prepared_target) {
	const std::uint64_t query_union_prepare_started_ns =
		static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
	prepared_target.release_query_union_state_noexcept();
	prepared_target.target_bp_size = target_bp_size;
	prepared_target.query_count = query_headers.size();
	prepared_target.physical_target_index_reads = 0;
	prepared_target.total_unique_keys = 0;
	prepared_target.key_set_checksum = 1469598103934665603ull;
	prepared_target.query_union_prepare_host_wall_ns = 0;
	prepared_target.unions_ready = false;
	prepared_target.query_union_prepare_host_wall_published = false;
	CpuQueryUnionStorage effective_union_storage = union_storage;
	if (pipe::vulkan_components_requested(base_params)) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Vulkan BatchJoin cannot use CPU target preparation"),
			 errdetail("biological Vulkan components were requested for this statement")));
	}
	if (query_recipe_datums.size() != query_headers.size()
		|| query_recipe_descriptors.size() != query_headers.size()
		|| query_ordinals.size() != query_headers.size()) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("BioseqBatchJoin query preparation columns disagree")));
	}
	std::vector<std::uint64_t> query_bp_sizes(query_headers.size(), 0u);
	bool has_recipe_query = false;
	for (std::size_t query_index = 0u;
		 query_index < query_headers.size(); ++query_index) {
		const bioseq::pg_types::seq* query_header = query_headers[query_index];
		const Datum recipe = query_recipe_datums[query_index];
		if (static_cast<unsigned>(query_header != nullptr)
				+ static_cast<unsigned>(recipe != Datum{0}) != 1u) {
			ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("BioseqBatchJoin query preparation ownership is not exclusive")));
		}
		if (query_header != nullptr) {
			if (query_header->alphabet_code != alphabet_code
				|| query_header->storage_unit_byte_width
					!= storage_unit_byte_width) {
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin query typmod changed during CPU preparation")));
			}
			query_bp_sizes[query_index] = query_header->bp_size;
		} else {
			has_recipe_query = true;
			const auto& descriptor = query_recipe_descriptors[query_index];
			const bool expanded = descriptor.kind
					== pg_sequence_datum_kind::expanded_read_only
				|| descriptor.kind
					== pg_sequence_datum_kind::expanded_read_write;
			if (query_read_session == nullptr || !expanded
				|| !descriptor.alphabet_bound
				|| descriptor.alphabet_code != alphabet_code) {
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin query recipe metadata is invalid during CPU preparation")));
			}
			query_bp_sizes[query_index] = descriptor.bp_length;
		}
		if (!minimizer_query_length_is_representable<MinimapSketchData>(
				query_bp_sizes[query_index])) {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BioseqBatchJoin query exceeds the 28-bit minimizer position domain")));
		}
	}
	for (std::size_t index = 1u; index < query_ordinals.size(); ++index) {
		if (query_ordinals[index - 1u] >= query_ordinals[index]) {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("BioseqBatchJoin query ordinals are not strictly ordered")));
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
	if (effective_union_storage == CpuQueryUnionStorage::canonical_u32
		|| effective_union_storage
			== CpuQueryUnionStorage::retained_sketch_only) {
		auto retained_query_permit =
			bioseq::parallelism::MemoryAdmissionController::instance().
				try_reserve(0u);
		if (retained_query_permit) {
			prepared_target.query_sketch_cache_permit.emplace(
				std::move(*retained_query_permit));
		}
	}
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
	bool used_parallel_canonical_preparation = false;
	bool parallel_group_is_uniform =
		(effective_union_storage == CpuQueryUnionStorage::canonical_u32
			|| effective_union_storage
				== CpuQueryUnionStorage::retained_sketch_only)
		&& include_persisted_parameter_group
		&& !has_recipe_query
		&& query_headers.size() >= 2u
		&& prepared_target.query_sketch_cache_permit.has_value();
	std::int32_t parallel_max_occ = 0;
	MinimizerSketchBackend parallel_sketch_backend =
		MinimizerSketchBackend::Scalar;
	for (std::size_t query_index = 0u;
		 parallel_group_is_uniform && query_index < query_headers.size();
		 ++query_index) {
		const Minimap2Params effective = effective_params_for_pair(
			base_params,
			std::min<std::uint64_t>(
				target_bp_size, query_bp_sizes[query_index]));
		const MinimizerSketchBackend sketch_backend =
			pipe::target_index_sketch_backend_from_params(effective);
		if (query_index == 0u) {
			parallel_max_occ = effective.max_occ;
			parallel_sketch_backend = sketch_backend;
		} else if (parallel_max_occ != effective.max_occ
			|| parallel_sketch_backend != sketch_backend) {
			parallel_group_is_uniform = false;
		}
	}
	auto& pool = bioseq::parallelism::GlobalPool::instance();
	parallel_group_is_uniform =
		parallel_group_is_uniform
		&& bioseq::parallelism::dispatch_parallel(pool.mode())
		&& pool.max_concurrency() > 1u;
	if (effective_union_storage == CpuQueryUnionStorage::retained_sketch_only
		&& !parallel_group_is_uniform) {
		/* Retention is an optimization, not a correctness prerequisite.  If
		 * full-batch retention cannot be proven before sketching starts, retain
		 * the historical admitted canonical owner and publish synchronously. */
		effective_union_storage = CpuQueryUnionStorage::canonical_u32;
	}
	if (parallel_group_is_uniform) {
		CpuPreparedIndexGroup& group = ensure_group(
			15, 10, parallel_max_occ, parallel_sketch_backend);
		if (!reserve_parallel_query_sketch_slots(
				group.retained_query_sketches,
				query_headers.size(),
				*prepared_target.query_sketch_cache_permit,
				prepared_target.query_sketch_cache_admission_bytes)) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("BioseqBatchJoin parallel query-sketch slots cannot be admitted")));
			});
			pg_unreachable();
		}
		std::vector<ParallelPreparedQuerySketchStatus> statuses(
			query_headers.size(),
			ParallelPreparedQuerySketchStatus::pending);
		std::vector<std::exception_ptr> exceptions(query_headers.size());
		pool.parallel_for_with_context_tracked(
			0u,
			query_headers.size(),
			1u,
			bioseq::parallelism::range_body{
				[&](const std::size_t lo, const std::size_t hi) {
					for (std::size_t query_index = lo;
						 query_index < hi;
						 ++query_index) {
						try {
							statuses[query_index] =
								dispatch_prepare_independently_admitted_query_sketch(
									alphabet_code,
									storage_unit_byte_width,
									*query_headers[query_index],
									query_ordinals[query_index],
									group.kmer_size,
									group.window_size,
									group.sketch_backend,
									group.retained_query_sketches[
										query_index]);
						} catch (...) {
							exceptions[query_index] =
								std::current_exception();
						}
					}
				}},
			bioseq::parallelism::current_tracked_job_observer());
		for (std::size_t query_index = 0u;
			 query_index < query_headers.size();
			 ++query_index) {
			if (exceptions[query_index]) {
				std::rethrow_exception(exceptions[query_index]);
			}
			switch (statuses[query_index]) {
				case ParallelPreparedQuerySketchStatus::ready:
					break;
				case ParallelPreparedQuerySketchStatus::
						query_length_unsupported:
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("BioseqBatchJoin query exceeds the 28-bit minimizer position domain")));
					});
					pg_unreachable();
				case ParallelPreparedQuerySketchStatus::
						admission_unavailable:
				case ParallelPreparedQuerySketchStatus::
						collector_growth_failed:
				case ParallelPreparedQuerySketchStatus::
						capacity_mismatch:
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("BioseqBatchJoin parallel query sketch exceeded admission")));
					});
					pg_unreachable();
				case ParallelPreparedQuerySketchStatus::proof_invalid:
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("BioseqBatchJoin parallel query sketch proof does not match the selected backend")));
					});
					pg_unreachable();
				case ParallelPreparedQuerySketchStatus::
						unsupported_typmod:
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("BioseqBatchJoin parallel query preparation received an unsupported typmod")));
					});
					pg_unreachable();
				case ParallelPreparedQuerySketchStatus::pending:
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("BioseqBatchJoin parallel query preparation left an incomplete slot")));
					});
					pg_unreachable();
			}
		}
		const bool materialize_canonical =
			effective_union_storage == CpuQueryUnionStorage::canonical_u32;
		std::size_t canonical_size = 0u;
		for (CpuPreparedQuerySketch& retained
			 : group.retained_query_sketches) {
			const std::uint64_t retained_minimizer_bytes =
				bioseq::parallelism::saturating_mul(
					retained.minimizers.capacity(),
					sizeof(MinimapSketchData));
			const std::uint64_t reconciled_cache_bytes =
				bioseq::parallelism::saturating_add(
					prepared_target.query_sketch_cache_admission_bytes,
					retained_minimizer_bytes);
			if (!retained.parallel_preparation_permit.has_value()
				|| retained_minimizer_bytes
					== (std::numeric_limits<std::uint64_t>::max)()
				|| retained_minimizer_bytes
					!= retained.parallel_preparation_permit->bytes()
				|| reconciled_cache_bytes
					== (std::numeric_limits<std::uint64_t>::max)()
				|| !prepared_target.query_sketch_cache_permit->absorb(
					std::move(*retained.parallel_preparation_permit))
				|| (materialize_canonical
					&& retained.minimizers.size()
						> group.canonical_query_hash_union_u32.max_size()
							- canonical_size)) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin parallel query cache cannot be consolidated")));
				});
				pg_unreachable();
			}
			retained.parallel_preparation_permit.reset();
			prepared_target.query_sketch_cache_admission_bytes =
				reconciled_cache_bytes;
			if (materialize_canonical) {
				canonical_size += retained.minimizers.size();
			}
		}
		if (materialize_canonical) {
			AdmittedCanonicalU32QueryCollector canonical_collector{
				group.canonical_query_hash_union_u32,
				*prepared_target.query_union_permit,
				prepared_target.query_union_admission_bytes};
			if (!canonical_collector.reserve(canonical_size)) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin parallel canonical query union cannot be admitted")));
				});
				pg_unreachable();
			}
			for (const CpuPreparedQuerySketch& retained
					 : group.retained_query_sketches) {
				for (const MinimapSketchData& minimizer
						 : retained.minimizers) {
					canonical_collector.collect(minimizer);
				}
			}
			if (!canonical_collector.hash_width_valid()) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("Format-6 canonical query hash exceeds its authenticated u32 profile")));
				});
				pg_unreachable();
			}
			if (!canonical_collector.ok()) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin parallel canonical query union growth cannot be admitted")));
				});
				pg_unreachable();
			}
		}
		used_parallel_canonical_preparation = true;
	}
	if (!used_parallel_canonical_preparation) {
		for (std::size_t query_index = 0u;
			 query_index < query_headers.size();
			 ++query_index) {
			const bioseq::pg_types::seq* const query_header = query_headers[query_index];
			const std::uint64_t query_ordinal = query_ordinals[query_index];
			const Minimap2Params effective = effective_params_for_pair(
				base_params,
				std::min<std::uint64_t>(
					target_bp_size, query_bp_sizes[query_index]));
			const MinimizerSketchBackend sketch_backend =
				pipe::target_index_sketch_backend_from_params(effective);
			auto append_to = [&](CpuPreparedIndexGroup& group) {
				if (effective_union_storage
						== CpuQueryUnionStorage::canonical_u32
					|| effective_union_storage
						== CpuQueryUnionStorage::
							canonical_u32_sequential_fallback) {
				const auto append_canonical =
					[query_ordinal, &group, &prepared_target](
						auto&& append_function) {
					append_function(
						query_ordinal,
						group.kmer_size,
						group.window_size,
						group.sketch_backend,
						group.canonical_query_hash_union_u32,
						*prepared_target.query_union_permit,
						prepared_target.query_union_admission_bytes,
						prepared_target.query_sketch_cache_permit
							? &group.retained_query_sketches
							: nullptr,
						prepared_target.query_sketch_cache_permit
							? &*prepared_target.
								query_sketch_cache_permit
							: nullptr,
						prepared_target.query_sketch_cache_permit
							? &prepared_target.
								query_sketch_cache_admission_bytes
							: nullptr);
				};
				if (query_header != nullptr) {
					append_canonical(
						[alphabet_code, storage_unit_byte_width, query_header](
							auto&&... arguments) {
						dispatch_append_cpu_prepared_query_sketch(
							alphabet_code, storage_unit_byte_width,
							*query_header,
							std::forward<decltype(arguments)>(arguments)...);
					});
				} else {
					append_canonical(
						[alphabet_code, query_recipe_datums,
						 query_recipe_descriptors, query_index,
						 query_read_session, query_payload_observer](
							auto&&... arguments) {
						dispatch_append_cpu_prepared_query_recipe_sketch(
							alphabet_code,
							query_recipe_datums[query_index],
							query_recipe_descriptors[query_index],
							*query_read_session,
							query_payload_observer,
							std::forward<decltype(arguments)>(arguments)...);
					});
				}
				} else {
					const auto append_union =
						[query_ordinal, &group, &prepared_target](
							auto&& append_function) {
						append_function(
						query_ordinal,
						group.kmer_size,
						group.window_size,
						group.sketch_backend,
						group.query_hash_union,
						*prepared_target.query_union_permit,
						prepared_target.query_union_admission_bytes);
					};
					if (query_header != nullptr) {
						append_union(
							[alphabet_code, storage_unit_byte_width,
							 query_header](auto&&... arguments) {
							dispatch_append_cpu_prepared_query_sketch(
								alphabet_code, storage_unit_byte_width,
								*query_header,
								std::forward<decltype(arguments)>(arguments)...);
						});
					} else {
						append_union(
							[alphabet_code, query_recipe_datums,
							 query_recipe_descriptors, query_index,
							 query_read_session, query_payload_observer](
								auto&&... arguments) {
							dispatch_append_cpu_prepared_query_recipe_sketch(
								alphabet_code,
								query_recipe_datums[query_index],
								query_recipe_descriptors[query_index],
								*query_read_session,
								query_payload_observer,
								std::forward<decltype(arguments)>(arguments)...);
						});
					}
				}
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
	}
	for (CpuPreparedIndexGroup& group : prepared_target.groups) {
		if (effective_union_storage
				== CpuQueryUnionStorage::retained_sketch_only) {
			bool retained_proofs_match =
				group.retained_query_sketches.size() == query_headers.size()
				&& group.query_hash_union.empty()
				&& group.query_hash_union.capacity() == 0u
				&& group.canonical_query_hash_union_u32.empty()
				&& group.canonical_query_hash_union_u32.capacity() == 0u;
			for (std::size_t query_index = 0u;
				 retained_proofs_match
					&& query_index < group.retained_query_sketches.size();
				 ++query_index) {
				const CpuPreparedQuerySketch& retained =
					group.retained_query_sketches[query_index];
				retained_proofs_match =
					retained.query_ordinal == query_ordinals[query_index]
					&& retained.query_bp_size
						== query_bp_sizes[query_index]
					&& pipe::minimizer_range_proof_matches_backend(
						group.sketch_backend,
						retained.proof,
						/*require_nonempty_vector_work=*/false,
						pipe::ExactSimdRangeRoute::require_continuation);
			}
			if (!retained_proofs_match) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("compact-direct retained-only query cache/proof is incomplete")));
				});
				pg_unreachable();
			}
			group.compact_direct_query_set_proof_pending = true;
			continue;
		}
		if (effective_union_storage == CpuQueryUnionStorage::canonical_u32
			|| effective_union_storage
				== CpuQueryUnionStorage::canonical_u32_sequential_fallback) {
			if (!group.query_hash_union.empty()
				|| group.query_hash_union.capacity() != 0u) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("Format-6 canonical query preparation retained a duplicate sketch owner")));
				});
				pg_unreachable();
			}
			for (const CpuPreparedQuerySketch& retained
				 : group.retained_query_sketches) {
				if (!pipe::minimizer_range_proof_matches_backend(
						group.sketch_backend,
						retained.proof,
						/*require_nonempty_vector_work=*/false,
						pipe::ExactSimdRangeRoute::
							require_continuation)) {
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Format-6 retained query sketch proof is invalid")));
					});
					pg_unreachable();
				}
			}
			auto& canonical = group.canonical_query_hash_union_u32;
			if (canonical.size() > 1u) {
				bioseq::sorting::radix_sort(
					canonical.data(), canonical.data() + canonical.size(),
					[](const std::uint32_t hash) noexcept {
						return hash;
					});
			}
			canonical.erase(
				std::unique(canonical.begin(), canonical.end()),
				canonical.end());
			const auto fnv_mix_u64 = [](
					std::uint64_t hash,
					std::uint64_t value) noexcept {
				for (int byte_index = 0; byte_index < 8; ++byte_index) {
					hash ^= value & 0xffu;
					hash *= 1099511628211ull;
					value >>= 8u;
				}
				return hash;
			};
			std::uint64_t signature = 1469598103934665603ull;
			signature = fnv_mix_u64(
				signature, static_cast<std::uint32_t>(group.kmer_size));
			signature = fnv_mix_u64(
				signature, static_cast<std::uint32_t>(group.window_size));
			signature = fnv_mix_u64(
				signature, static_cast<std::uint32_t>(group.max_occ));
			for (const std::uint32_t hash : canonical) {
				signature = fnv_mix_u64(signature, hash);
			}
			group.key_set_checksum = signature;
			prepared_target.total_unique_keys =
				bioseq::parallelism::saturating_add(
					prepared_target.total_unique_keys,
					static_cast<std::uint64_t>(canonical.size()));
			auto mix = [&prepared_target](
					const std::uint64_t value) noexcept {
				prepared_target.key_set_checksum ^= value;
				prepared_target.key_set_checksum *= 1099511628211ull;
			};
			mix(static_cast<std::uint64_t>(group.kmer_size));
			mix(static_cast<std::uint64_t>(group.window_size));
			mix(static_cast<std::uint64_t>(group.max_occ));
			mix(static_cast<std::uint64_t>(group.sketch_backend));
			mix(static_cast<std::uint64_t>(canonical.size()));
			mix(group.key_set_checksum);
			continue;
		}
		if (!group.canonical_query_hash_union_u32.empty()
			|| group.canonical_query_hash_union_u32.capacity() != 0u) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("legacy/F5 query preparation retained a duplicate canonical u32 owner")));
			});
			pg_unreachable();
		}
		if (!group.retained_query_sketches.empty()
			|| group.retained_query_sketches.capacity() != 0u) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("legacy/F5 query preparation retained a Format-6 query cache")));
			});
			pg_unreachable();
		}
		/* The target-wide union has tens of millions of trivially-copyable
		 * sketches on the production workload.  The existing in-place radix
		 * implementation avoids comparison-sort's O(Q log Q) key extraction
		 * without introducing another full-size owner or admission peak. */
		if (group.query_hash_union.size() > 1u) {
			std::uint64_t maximum_hash = 0u;
			for (const MinimapSketchData& minimizer
					: group.query_hash_union) {
				maximum_hash = std::max(maximum_hash, minimizer.kmer());
			}
			if (maximum_hash
					<= (std::numeric_limits<std::uint32_t>::max)()) {
				/* Every currently registered persisted DNA4 profile is at
				 * most 32 bits. Returning the proven narrower key avoids four
				 * known-zero radix passes while the fallback below keeps the
				 * generic 35-bit MinimapSketchData contract intact. */
				bioseq::sorting::radix_sort(
					group.query_hash_union.data(),
					group.query_hash_union.data()
						+ group.query_hash_union.size(),
					[](const MinimapSketchData& minimizer) noexcept
							-> std::uint32_t {
						return static_cast<std::uint32_t>(minimizer.kmer());
					});
			} else {
				bioseq::sorting::radix_sort(
					group.query_hash_union.data(),
					group.query_hash_union.data()
						+ group.query_hash_union.size(),
					[](const MinimapSketchData& minimizer) noexcept
							-> std::uint64_t {
						return minimizer.kmer();
					});
			}
		}
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
		measured_query_union_bytes =
			bioseq::parallelism::saturating_add(
				measured_query_union_bytes,
				bioseq::parallelism::saturating_mul(
					group.canonical_query_hash_union_u32.capacity(),
					static_cast<std::uint64_t>(
						sizeof(std::uint32_t))));
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
	std::uint64_t measured_query_sketch_cache_bytes = 0u;
	for (const CpuPreparedIndexGroup& group : prepared_target.groups) {
		measured_query_sketch_cache_bytes =
			bioseq::parallelism::saturating_add(
				measured_query_sketch_cache_bytes,
				bioseq::parallelism::saturating_mul(
					group.retained_query_sketches.capacity(),
					static_cast<std::uint64_t>(
						sizeof(CpuPreparedQuerySketch))));
		for (const CpuPreparedQuerySketch& retained
			 : group.retained_query_sketches) {
			measured_query_sketch_cache_bytes =
				bioseq::parallelism::saturating_add(
					measured_query_sketch_cache_bytes,
					bioseq::parallelism::saturating_mul(
						retained.minimizers.capacity(),
						static_cast<std::uint64_t>(
							sizeof(MinimapSketchData))));
		}
	}
	if (measured_query_sketch_cache_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_query_sketch_cache_bytes
			!= prepared_target.query_sketch_cache_admission_bytes
		|| (prepared_target.query_sketch_cache_permit.has_value()
			? measured_query_sketch_cache_bytes
				> prepared_target.query_sketch_cache_permit->bytes()
			: measured_query_sketch_cache_bytes != 0u)) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("BioseqBatchJoin retained query cache exceeded admission")));
		});
		pg_unreachable();
	}
	if (prepared_target.query_sketch_cache_permit) {
		prepared_target.query_sketch_cache_permit->shrink_to(
			measured_query_sketch_cache_bytes);
	}
	const std::uint64_t query_union_prepare_finished_ns =
		static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
	prepared_target.query_union_prepare_host_wall_ns =
		query_union_prepare_finished_ns >= query_union_prepare_started_ns
			? std::max<std::uint64_t>(
				1u,
				query_union_prepare_finished_ns
					- query_union_prepare_started_ns)
			: 1u;
	prepared_target.unions_ready = true;
}

bool BatchProcessor::publish_compact_direct_query_set_proof(
		CpuPreparedTarget& prepared_target,
		CpuPreparedIndexGroup& group,
		const bioseq::indexing::CompactDirectQuerySetProof& proof) noexcept {
	if (!prepared_target.unions_ready
		|| prepared_target.groups.size() != 1u
		|| &prepared_target.groups.front() != &group
		|| !group.compact_direct_query_set_proof_pending
		|| group.max_occ <= 0
		|| !proof.present
		|| proof.query_count != prepared_target.query_count
		|| proof.query_count != group.retained_query_sketches.size()
		|| proof.unique_hash_count > proof.query_occurrence_count
		|| !group.query_hash_union.empty()
		|| group.query_hash_union.capacity() != 0u
		|| !group.canonical_query_hash_union_u32.empty()
		|| group.canonical_query_hash_union_u32.capacity() != 0u
		|| prepared_target.total_unique_keys != 0u
		|| prepared_target.key_set_checksum
			!= bioseq::indexing::kCompactDirectQuerySetFnvOffset
		|| group.key_set_checksum != 0u
		|| group.build_set_checksum_present
		|| group.build_set_checksum != 0u) {
		return false;
	}

	std::uint64_t retained_occurrence_count = 0u;
	for (const CpuPreparedQuerySketch& retained
			: group.retained_query_sketches) {
		retained_occurrence_count = bioseq::parallelism::saturating_add(
			retained_occurrence_count,
			static_cast<std::uint64_t>(retained.minimizers.size()));
	}
	if (retained_occurrence_count
			== (std::numeric_limits<std::uint64_t>::max)()
		|| retained_occurrence_count != proof.query_occurrence_count) {
		return false;
	}

	group.key_set_checksum = proof.group_key_set_checksum;
	group.build_set_checksum = proof.group_key_set_checksum;
	group.build_set_checksum_present = true;
	prepared_target.total_unique_keys = proof.unique_hash_count;
	auto mix = [&prepared_target](const std::uint64_t value) noexcept {
		prepared_target.key_set_checksum ^= value;
		prepared_target.key_set_checksum *=
			bioseq::indexing::kCompactDirectQuerySetFnvPrime;
	};
	mix(static_cast<std::uint64_t>(group.kmer_size));
	mix(static_cast<std::uint64_t>(group.window_size));
	mix(static_cast<std::uint64_t>(group.max_occ));
	mix(static_cast<std::uint64_t>(group.sketch_backend));
	mix(proof.unique_hash_count);
	mix(proof.group_key_set_checksum);
	group.compact_direct_query_set_proof_pending = false;
	return true;
}

void BatchProcessor::finalize_pending_compact_direct_query_set_proof_fallback(
		CpuPreparedTarget& prepared_target) {
	if (prepared_target.query_set_proof_ready()) return;
	if (!prepared_target.unions_ready
		|| prepared_target.groups.size() != 1u) {
		throw bioseq::indexing::PersistedMinimizerLookupError(
			"compact-direct query-set fallback has no exclusive prepared group");
	}
	CpuPreparedIndexGroup& group = prepared_target.groups.front();
	if (!group.compact_direct_query_set_proof_pending
		|| group.max_occ <= 0
		|| group.retained_query_sketches.size()
			!= prepared_target.query_count
		|| !group.query_hash_union.empty()
		|| group.query_hash_union.capacity() != 0u
		|| !group.canonical_query_hash_union_u32.empty()
		|| group.canonical_query_hash_union_u32.capacity() != 0u) {
		throw bioseq::indexing::PersistedMinimizerLookupError(
			"compact-direct query-set fallback retained state is incomplete");
	}

	std::uint64_t occurrence_count = 0u;
	std::optional<std::uint64_t> previous_query_ordinal;
	for (const CpuPreparedQuerySketch& retained
			: group.retained_query_sketches) {
		if ((previous_query_ordinal
				&& *previous_query_ordinal >= retained.query_ordinal)
			|| !pipe::minimizer_range_proof_matches_backend(
				group.sketch_backend,
				retained.proof,
				/*require_nonempty_vector_work=*/false,
				pipe::ExactSimdRangeRoute::require_continuation)) {
			throw bioseq::indexing::PersistedMinimizerLookupError(
				"compact-direct query-set fallback retained proof is invalid");
		}
		previous_query_ordinal = retained.query_ordinal;
		occurrence_count = bioseq::parallelism::saturating_add(
			occurrence_count,
			static_cast<std::uint64_t>(retained.minimizers.size()));
	}
	const std::uint64_t requested_bytes =
		bioseq::parallelism::saturating_mul(
			occurrence_count, sizeof(std::uint32_t));
	const std::uint64_t allocation_headroom =
		bioseq::parallelism::saturating_mul(
			requested_bytes,
			bioseq::parallelism::kVectorPreallocationHeadroomFactor);
	if (occurrence_count
			== (std::numeric_limits<std::uint64_t>::max)()
		|| occurrence_count
			> (std::numeric_limits<std::size_t>::max)()
		|| requested_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| allocation_headroom
			== (std::numeric_limits<std::uint64_t>::max)()) {
		throw bioseq::indexing::PersistedMinimizerLookupError(
			"compact-direct query-set fallback size overflow");
	}
	auto fallback_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			allocation_headroom);
	if (!fallback_permit) {
		throw bioseq::indexing::PersistedMinimizerLookupError(
			bioseq::indexing::PersistedMinimizerLookupError::Reason::
				admission_or_allocation,
			"compact-direct query-set fallback cannot be admitted");
	}
	std::vector<std::uint32_t> canonical;
	try {
		canonical.reserve(static_cast<std::size_t>(occurrence_count));
		const std::uint64_t measured_bytes =
			bioseq::parallelism::saturating_mul(
				canonical.capacity(), sizeof(std::uint32_t));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > fallback_permit->bytes()) {
			throw bioseq::indexing::PersistedMinimizerLookupError(
				bioseq::indexing::PersistedMinimizerLookupError::Reason::
					admission_or_allocation,
				"compact-direct query-set fallback exceeded admission");
		}
		fallback_permit->shrink_to(measured_bytes);
		for (const CpuPreparedQuerySketch& retained
				: group.retained_query_sketches) {
			for (const MinimapSketchData& minimizer : retained.minimizers) {
				if (minimizer.kmer()
						> (std::numeric_limits<std::uint32_t>::max)()) {
					throw bioseq::indexing::PersistedMinimizerLookupError(
						"compact-direct query-set fallback hash exceeds u32");
				}
				canonical.push_back(
					static_cast<std::uint32_t>(minimizer.kmer()));
			}
		}
		if (canonical.size() > 1u) {
			bioseq::sorting::radix_sort(
				canonical.data(), canonical.data() + canonical.size(),
				[](const std::uint32_t hash) noexcept { return hash; });
		}
		canonical.erase(
			std::unique(canonical.begin(), canonical.end()),
			canonical.end());
		const bioseq::indexing::CompactDirectQuerySetProof proof{
			.query_count = prepared_target.query_count,
			.query_occurrence_count = occurrence_count,
			.unique_hash_count = canonical.size(),
			.group_key_set_checksum =
				bioseq::indexing::compact_direct_query_set_checksum(
					canonical,
					static_cast<std::uint32_t>(group.kmer_size),
					static_cast<std::uint32_t>(group.window_size),
					static_cast<std::uint32_t>(group.max_occ)),
			.present = true,
		};
		if (!publish_compact_direct_query_set_proof(
				prepared_target, group, proof)) {
			throw bioseq::indexing::PersistedMinimizerLookupError(
				"compact-direct query-set fallback publication was rejected");
		}
	} catch (const std::bad_alloc&) {
		throw bioseq::indexing::PersistedMinimizerLookupError(
			bioseq::indexing::PersistedMinimizerLookupError::Reason::
				admission_or_allocation,
			"compact-direct query-set fallback allocation failed");
	}
}

void BatchProcessor::prepare_cpu_persisted_native_target(
		const Oid database_oid,
		const Oid tablespace_oid,
		const Oid main_relfilenode,
		const Oid user_relid,
		const std::int16_t column_attno,
		const std::string_view index_name,
		const std::uint64_t sequence_rowno,
		const std::uint64_t target_bp_size,
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const std::span<const bioseq::pg_types::seq* const> query_headers,
		const std::span<const Datum> query_recipe_datums,
		const std::span<const pg_sequence_descriptor>
			query_recipe_descriptors,
		pg_sequence_read_session* query_read_session,
		const bioseq::storage::sequence_payload_allocation_observer*
			query_payload_observer,
		const std::span<const std::uint64_t> query_ordinals,
		const Minimap2Params& base_params,
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
	if (prepared_target.unions_ready
		|| prepared_target.target_rowno < 0
		|| static_cast<std::uint64_t>(prepared_target.target_rowno)
			!= sequence_rowno
		|| !prepared_target.groups.empty()
		|| prepared_target.query_union_permit.has_value()
		|| prepared_target.query_union_admission_bytes != 0u
		|| prepared_target.query_sketch_cache_permit.has_value()
		|| prepared_target.query_sketch_cache_admission_bytes != 0u
		|| target_bp_size == 0u
		|| query_headers.empty()
		|| query_recipe_datums.size() != query_headers.size()
		|| query_recipe_descriptors.size() != query_headers.size()
		|| query_ordinals.size() != query_headers.size()
		|| database_oid != MyDatabaseId
		|| !OidIsValid(user_relid)
		|| !OidIsValid(tablespace_oid)
		|| !OidIsValid(main_relfilenode)
		|| column_attno <= 0
		|| !executor_read.matches(
			database_oid, tablespace_oid, main_relfilenode, user_relid)) {
		fail("CPU native persisted prepared-target identity/lifecycle is invalid");
		return;
	}
	const MinimizerSketchBackend sketch_backend =
		pipe::target_index_sketch_backend_from_params(base_params);

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
	const std::uint64_t source_content_checksum =
		source_proof.source_content_checksum_for_diagnostics();
	if (source.sequence_rowno != sequence_rowno
		|| source.locator.total_bp_size != target_bp_size
		|| source.alphabet_code != alphabet_code
		|| source.storage_unit_byte_width != storage_unit_byte_width) {
		fail("authoritative source identity disagrees with the prepared target");
		return;
	}
	identity.table_name = format_seqidx_name(
		index_name, database_oid, tablespace_oid, main_relfilenode,
		column_attno, source.locator.bucket_idx, source.locator.shard_idx);

	auto opened_format6 = open_exact_minimizer_format6_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read);
	if (!opened_format6.session && !opened_format6.head_absent) {
		switch (opened_format6.relation_failure.kind) {
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
						opened_format6.relation_failure.revision3_error)));
				return;
			case MinimizerGenerationRelationKind::revision3_exact:
				fail("Format-6 relation binder returned an invalid success classification");
				return;
			}
	}
	if (opened_format6.session && opened_format6.head_absent) {
		fail("Format-6 binder returned both a session and an absence proof");
		return;
	}
	bool format6_profile_applicable = false;
	if (opened_format6.session) {
		IndexStorageMinimizerFormat6ReadSessionPg& format6_session =
			*opened_format6.session;
		const auto scheme =
			bioseq::indexing::minimizer_scheme_identity_for_backend(
				sketch_backend);
		const auto producer =
			bioseq::indexing::
				minimizer_producer_backend_identity_for_backend(
					sketch_backend);
		const auto format6_candidate_manifest =
			format6_session.manifest();
		const auto compatibility =
			format6_candidate_manifest.compatibility;
		if (!bioseq::indexing::valid_minimizer_format6_compatibility(
				compatibility, format6_candidate_manifest.target_length)) {
			fail("Format-6 READY head names an invalid compatibility profile");
			return;
		}
		format6_profile_applicable =
			compatibility.hash_scheme_id == scheme.id
			&& compatibility.hash_scheme_version == scheme.version
			&& compatibility.alphabet_code == alphabet_code
			&& compatibility.kmer_size == 15u
			&& compatibility.window_size == 10u
			&& compatibility.hash_width_bytes == sizeof(std::uint32_t)
			&& compatibility.producer_backend_id
				== static_cast<std::uint16_t>(producer.id)
			&& compatibility.producer_backend_version == producer.version;
		if (!format6_profile_applicable) {
			/* A self-authenticated but route-inapplicable Format-6 profile is
			 * the only present-head fallback case.  No source proof has been
			 * consumed and no directory/control/posting callback has run, so
			 * the independent READY Format-5 sentinel may still be opened. */
			format6_session.close_metadata_read();
			opened_format6.session.reset();
		} else {
			const bioseq::indexing::MinimizerFormat6ExpectedGeneration
				format6_expected =
					format6_session.validate_source_attestation(
						std::move(source_proof));
			if (format6_expected.generation_id == 0u
			|| format6_expected.query_key != sequence_rowno
			|| format6_expected.source_epoch != source.locator.source_epoch
			|| format6_expected.source_content_crc64
				!= source_content_checksum
			|| format6_expected.target_length != target_bp_size
			|| format6_expected.compatibility != compatibility) {
				fail("Format-6 authenticated source binding disagrees with the active CPU route");
				return;
			}
			prepare_cpu_shared_query_unions(
				alphabet_code,
				storage_unit_byte_width,
				target_bp_size,
				query_headers,
				query_recipe_datums,
				query_recipe_descriptors,
				query_read_session,
				query_payload_observer,
				query_ordinals,
				base_params,
				/*include_persisted_parameter_group=*/true,
				CpuQueryUnionStorage::canonical_u32,
				prepared_target);
			if (!prepared_target.unions_ready
				|| prepared_target.groups.size() != 1u) {
				fail("Format-6 canonical query preparation did not yield one group");
				return;
			}
			CpuPreparedIndexGroup& format6_group =
				prepared_target.groups.front();
			if (format6_group.kmer_size != 15
				|| format6_group.window_size != 10
				|| format6_group.max_occ < 0
				|| format6_group.sketch_backend != sketch_backend
				|| !format6_group.query_hash_union.empty()
				|| format6_group.query_hash_union.capacity() != 0u
				|| format6_group.persisted_grouped32_reader
				|| format6_group.persisted_shape_v5_mapping_guard
				|| format6_group.persisted_shape_v5_reader
				|| format6_group.persisted_mps_shape_v5_reader
				|| format6_group.persisted_format6_reader
				|| format6_group.target_index) {
				fail("Format-6 canonical query group violates exclusive ownership");
				return;
			}
			const bioseq::indexing::CanonicalQueryHashView canonical_view{
				.values32 = std::span<const std::uint32_t>{
					format6_group.canonical_query_hash_union_u32},
				.width_bytes = sizeof(std::uint32_t),
				.storage_width_bytes = sizeof(std::uint32_t),
				.compatibility_digest =
					bioseq::indexing::
						minimizer_format6_compatibility_digest(
							compatibility),
			};
			const std::uint64_t format6_load_started_ns =
				static_cast<std::uint64_t>(
					bioseq::instr::clock::now_ns());
			auto format6_reader =
				bioseq::indexing::MinimizerFormat6SelectiveReader::open(
					bioseq::parallelism::MemoryAdmissionController::instance(),
					format6_session.encoded_head(),
					format6_session.encoded_manifest(),
					format6_expected,
					format6_session.source_ref(),
					canonical_view,
					static_cast<std::uint64_t>(format6_group.max_occ));
			if (!format6_reader) {
				fail(fmt::format(
					"Format-6 selective reader rejected authenticated storage error={}",
					static_cast<unsigned>(format6_reader.error())));
				return;
			}
			const auto format6_head = format6_session.head();
			const auto format6_manifest = format6_session.manifest();
			MinimizerGenerationReadStats read_stats =
				format6_session.finish();
			const std::uint64_t format6_load_finished_ns =
				static_cast<std::uint64_t>(
					bioseq::instr::clock::now_ns());
			const std::uint64_t format6_load_ns =
				format6_load_finished_ns >= format6_load_started_ns
					? std::max<std::uint64_t>(
						1u,
						format6_load_finished_ns
							- format6_load_started_ns)
					: 1u;
			bioseq_log<DBG>(
				"[bioseq] Format-6 selective reader: load_ns={} "
				"rows={} payload_bytes={} retained_positions={} "
				"stored_position_width={}",
				static_cast<unsigned long long>(format6_load_ns),
				static_cast<unsigned long long>(
					read_stats.format6.directory_rows
					+ read_stats.format6.control_rows
					+ read_stats.format6.posting_rows),
				static_cast<unsigned long long>(
					read_stats.format6.directory_bytes
					+ read_stats.format6.control_bytes
					+ read_stats.format6.posting_bytes),
				static_cast<unsigned long long>(
					format6_reader->memory_evidence()
						.retained_position_count),
				static_cast<unsigned>(
					format6_manifest.compatibility
						.stored_posting_width_bytes));
			const auto& memory_evidence =
				format6_reader->memory_evidence();
		const std::uint64_t peak_admitted_bytes = std::max({
			memory_evidence.directory_phase_peak_admitted_bytes,
			memory_evidence.control_phase_peak_admitted_bytes,
			memory_evidence.posting_phase_peak_admitted_bytes,
		});
		const std::uint64_t admitted_bytes =
			format6_reader->admitted_bytes();
		const std::uint64_t authorized_posting_bytes =
			bioseq::parallelism::saturating_mul(
				memory_evidence.retained_position_count,
				format6_manifest.compatibility
					.stored_posting_width_bytes);
		if (format6_reader->target_length() != target_bp_size
			|| format6_head.generation_id != format6_expected.generation_id
			|| format6_head.source_identity
				!= format6_manifest.source_identity
			|| format6_manifest.compatibility != compatibility
			|| read_stats.relation_opens != 1u
			|| read_stats.head_scans != 1u
			|| read_stats.source_parent_rows_read != 1u
			|| read_stats.source_catalog_rows_read != 1u
			|| read_stats.source_catalog_index_scans != 1u
			|| read_stats.source_fragment_rows_read != 0u
			|| read_stats.source_decoded_bp != 0u
			|| read_stats.format6.format_version
				!= bioseq::indexing::kMinimizerFormat6Version
			|| read_stats.format6.sessions != 1u
			|| read_stats.format6.generation_id
				!= format6_head.generation_id
			|| read_stats.format6.source_identity
				!= format6_head.source_identity
			|| read_stats.format6.source_content_crc64
				!= format6_head.source_content_crc64
			|| read_stats.format6.compatibility_digest
				!= format6_head.compatibility_digest
			|| memory_evidence.directory_stream_crc_receipts != 1u
			|| memory_evidence.control_block_crc_receipts
				!= memory_evidence.selected_block_count
			|| memory_evidence.control_semantic_receipts
				!= memory_evidence.selected_block_count
			|| memory_evidence.authorized_posting_bytes
				!= authorized_posting_bytes
			|| memory_evidence.generic_source_fallback_calls != 0u
			|| memory_evidence.peak_admitted_bytes
				!= peak_admitted_bytes
			|| memory_evidence.retained_owner_bytes != admitted_bytes
			|| read_stats.format6.directory_row_crc_receipts
				!= read_stats.format6.directory_rows
			|| read_stats.format6.control_row_crc_receipts
				!= read_stats.format6.control_rows
			|| read_stats.format6.posting_row_crc_receipts
				!= read_stats.format6.posting_rows
			|| read_stats.format6.posting_ranges_delivered
				!= memory_evidence.posting_ranges_delivered
			|| read_stats.format6.authorized_posting_bytes
				!= memory_evidence.authorized_posting_bytes
			|| memory_evidence.posting_semantic_receipts
				!= memory_evidence.posting_ranges_delivered
			|| memory_evidence.posting_range_crc_receipts
				> memory_evidence.posting_ranges_delivered
			|| !source_counters.timed_equation_holds()) {
			fail("Format-6 READY identity/profile/read-work proof disagrees");
			return;
		}
		auto& format6_evidence = read_stats.format6;
		format6_evidence.format_version =
			bioseq::indexing::kMinimizerFormat6Version;
		format6_evidence.sessions = 1u;
		format6_evidence.generation_id = format6_head.generation_id;
		format6_evidence.source_identity =
			format6_head.source_identity;
		format6_evidence.source_content_crc64 =
			format6_head.source_content_crc64;
		format6_evidence.compatibility_digest =
			format6_head.compatibility_digest;
		format6_evidence.directory_stream_crc_receipts =
			memory_evidence.directory_stream_crc_receipts;
		format6_evidence.control_block_crc_receipts =
			memory_evidence.control_block_crc_receipts;
		format6_evidence.control_semantic_receipts =
			memory_evidence.control_semantic_receipts;
		format6_evidence.posting_range_crc_receipts =
			memory_evidence.posting_range_crc_receipts;
		format6_evidence.posting_semantic_receipts =
			memory_evidence.posting_semantic_receipts;
		format6_evidence.query_count =
			memory_evidence.query_count;
		format6_evidence.selected_block_count =
			memory_evidence.selected_block_count;
		format6_evidence.accepted_hash_count =
			memory_evidence.accepted_hash_count;
		format6_evidence.retained_position_count =
			memory_evidence.retained_position_count;
		format6_evidence.directory_phase_peak_bytes =
			memory_evidence.directory_phase_peak_admitted_bytes;
		format6_evidence.control_phase_peak_bytes =
			memory_evidence.control_phase_peak_admitted_bytes;
		format6_evidence.posting_phase_peak_bytes =
			memory_evidence.posting_phase_peak_admitted_bytes;
		format6_evidence.peak_admitted_bytes =
			memory_evidence.peak_admitted_bytes;
		format6_evidence.retained_owner_bytes =
			memory_evidence.retained_owner_bytes;
		format6_evidence.admitted_bytes = admitted_bytes;
		format6_evidence.generic_source_fallback_calls =
			memory_evidence.generic_source_fallback_calls;
		format6_evidence.failures = 0u;
		format6_evidence.saturated = false;
		format6_group.persisted_format6_reader.emplace(
			std::move(*format6_reader));
		format6_group.target_index_source =
			::bioseq::pipeline_stats::TargetIndexSource::persisted;
		format6_group.persisted_generation_id =
			format6_head.generation_id;
		format6_group.persisted_format6_source_identity =
			format6_head.source_identity;
		format6_group.persisted_format6_source_identity_present = true;
		/* The legacy u64 pipeline correlation value is deliberately not used
		 * to authenticate Format-6; correctness above is bound to all 128
		 * source-identity bits. */
		format6_group.persisted_source_identity =
			format6_source_diagnostic_token(
				format6_head.source_identity);
		format6_group.persisted_read_stats = read_stats;
		format6_group.persisted_reader_loads = 1u;
		format6_group.build_set_checksum_present = true;
		format6_group.build_set_checksum =
			format6_group.key_set_checksum;
		++prepared_target.physical_target_index_reads;
		return;
		}
	}
	/* The binder's head_absent result is an authenticated absence of the
	 * distinct Format-6 sentinel.  Together with the inapplicable-profile case
	 * above, this is before every F6 stream payload callback and therefore the
	 * only clean boundary at which Format-5 may be tried. */
	if (!format6_f5_fallback_allowed(
			opened_format6.head_absent,
			format6_profile_applicable,
			/*stream_io_started=*/false)) {
		fail("Format-6 dispatch crossed the fail-closed fallback boundary");
		return;
	}
	opened_format6.session.reset();
	const auto expected =
		bioseq::indexing::expected_minimizer_compatibility_for_backend(
			MinimizerGenerationWireFormat::
				grouped_shape_soa_segment_local,
			sketch_backend,
			source.alphabet_code,
			source.storage_unit_byte_width,
			15u,
			10u,
			bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap);
	if (!expected) {
		fail(fmt::format("grouped32 compatibility rejected wire_error={}",
			static_cast<unsigned>(expected.error())));
		return;
	}
	const auto format4_expected =
		bioseq::indexing::expected_minimizer_compatibility_for_backend(
			MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
			sketch_backend,
			source.alphabet_code,
			source.storage_unit_byte_width,
			15u,
			10u);
	if (!format4_expected) {
		fail(fmt::format(
			"Format-4 compatibility rejected wire_error={}",
			static_cast<unsigned>(format4_expected.error())));
		return;
	}
	const auto configured_mpf_format5_lookup_route =
		decode_mpf_format5_lookup_route(
			static_cast<int>(bioseq::aligner::settings::mpf_format5_lookup_route.value()));
	if (!configured_mpf_format5_lookup_route) {
		fail("MPF Format-5 lookup-route GUC has an invalid wire value");
		return;
	}
	const bool compact_h32_direct_configured =
		*configured_mpf_format5_lookup_route
		== MpfFormat5LookupRoute::compact_h32_direct;
	const bool compact_h32_direct_requested =
		compact_h32_direct_configured
		&& index_name
			== bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
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
	const auto head =
		session.read_active_head_allowing_format4_compatibility_once(
			*format4_expected);
	std::optional<std::span<const std::byte>>
		authenticated_format5_directory{};
	const MinimizerGenerationReadMetadata* metadata = nullptr;
	MinimizerGenerationManifest manifest{};
	if (compact_h32_direct_requested) {
		manifest = session.read_manifest_once_without_directory(head);
	} else {
		metadata = &session.read_manifest_directory_once(head);
		manifest = metadata->manifest;
		authenticated_format5_directory =
			metadata->directory.encoded_bytes();
	}
	const bool format5 = manifest.wire_format
		== MinimizerGenerationWireFormat::grouped_shape_soa_segment_local;
	const bool mapped_mpf_format5 = format5
		&& index_name
			== bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
	const bool full_mps_format5 = format5
		&& index_name
			== bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
	if (format5 && !mapped_mpf_format5 && !full_mps_format5) {
		fail("Format-5 READY generation is bound to an unsupported logical index");
		return;
	}
	if (compact_h32_direct_requested && !mapped_mpf_format5) {
		fail("compact-H32 direct lookup requires an MPF Format-5 READY generation");
		return;
	}
	const auto& selected_expected = manifest.wire_format
			== MinimizerGenerationWireFormat::
				grouped_shape_soa_segment_local
		? *expected : *format4_expected;
	session.validate_source_attestation(std::move(source_proof));
	auto& query_prep_pool = bioseq::parallelism::GlobalPool::instance();
	/* This is the exact eligibility boundary of the typed full-batch native
	 * route.  Outside it, preserve the synchronous canonical-u32 proof because
	 * no later complete QueryHashPlan stream is guaranteed to exist. */
	const bool defer_compact_direct_query_set_proof =
		compact_h32_direct_requested
		&& base_params.max_occ > 0
		&& !base_params.enable_profiling
		&& target_bp_size > UINT64_C(256000000)
		&& std::ranges::all_of(
			query_recipe_datums,
			[](Datum datum) noexcept { return datum == Datum{0}; })
		&& query_headers.size()
			>= bioseq::parallelism::TaskParallelMinTriggers::MIN_QUERY_COUNT
		&& bioseq::parallelism::task_parallel(query_prep_pool.mode())
		&& query_prep_pool.max_concurrency() > 1u;
	prepare_cpu_shared_query_unions(
		alphabet_code,
		storage_unit_byte_width,
		target_bp_size,
		query_headers,
		query_recipe_datums,
		query_recipe_descriptors,
		query_read_session,
		query_payload_observer,
		query_ordinals,
		base_params,
		/*include_persisted_parameter_group=*/true,
		mapped_mpf_format5
			? (defer_compact_direct_query_set_proof
				? CpuQueryUnionStorage::retained_sketch_only
				: CpuQueryUnionStorage::canonical_u32)
			: CpuQueryUnionStorage::minimizer_sketch,
		prepared_target);
	if (!prepared_target.unions_ready
		|| prepared_target.groups.size() != 1u) {
		fail("legacy/F5 query preparation did not yield one group");
		return;
	}
	CpuPreparedIndexGroup& group = prepared_target.groups.front();
	if (group.kmer_size != 15 || group.window_size != 10
		|| group.max_occ < 0
		|| group.sketch_backend != sketch_backend
		|| (group.compact_direct_query_set_proof_pending
			&& (!defer_compact_direct_query_set_proof
				|| group.max_occ <= 0
				|| !group.canonical_query_hash_union_u32.empty()
				|| group.canonical_query_hash_union_u32.capacity() != 0u))
		|| (mapped_mpf_format5
			? (!group.query_hash_union.empty()
				|| group.query_hash_union.capacity() != 0u)
			: (!group.canonical_query_hash_union_u32.empty()
				|| group.canonical_query_hash_union_u32.capacity() != 0u))
		|| group.persisted_grouped32_reader
		|| group.persisted_shape_v5_mapping_guard
		|| group.persisted_shape_v5_reader
		|| group.persisted_mps_shape_v5_reader
		|| group.persisted_format6_reader
		|| group.target_index) {
		fail("legacy/F5 parameter group is not the pinned exclusive cold-read shape");
		return;
	}
	// MPF Format 5 remains a collection of independent physical-fragment
	// indexes. The mmap image is the byte-exact artifact produced by the
	// writer's complete authenticated READY reread, fsync, and durable rename.
	// Cold open checks the exact generation identity and prepares only fragment
	// address plans. Query-wave max-occ reduction is fused into the parallel
	// fragment probe; no target-wide posting/count owner is constructed.
	if (mapped_mpf_format5) {
		using ShapeReader =
			bioseq::indexing::PreparedShapeV5FragmentQueryReader;
		const std::uint64_t shape_v5_load_started_ns =
			static_cast<std::uint64_t>(
				bioseq::instr::clock::now_ns());
		const auto storage_descriptor =
			session.read_live_storage_descriptor_once(identity);
		auto mapped_image = open_exact_minimizer_format5_image_pg(
			identity, sequence_rowno, head, manifest,
			storage_descriptor,
			compact_h32_direct_requested
				? Mpf5V2MappedOpenValidationPg::bounded_header_only
				: Mpf5V2MappedOpenValidationPg::full_metadata);
		std::optional<ShapeReader> shape_reader{};
		MinimizerGenerationReadStats read_stats{};
		if (compact_h32_direct_requested) {
			const auto direct_profile =
				bioseq::indexing::minimizer_generation_payload_profile(
					manifest);
			if (!direct_profile || !direct_profile->shape_v5
				|| direct_profile->max_hash == 0u
				|| direct_profile->max_hash
					> (std::numeric_limits<std::uint32_t>::max)()
				|| direct_profile->shape_v5->max_occ_cap == 0u
				|| direct_profile->shape_v5->max_occ_cap
					> (std::numeric_limits<std::uint32_t>::max)()) {
				fail("compact-H32 direct profile geometry is invalid");
				return;
			}
			const std::uint32_t profile_max_hash =
				static_cast<std::uint32_t>(direct_profile->max_hash);
			const std::uint32_t profile_hash_bits =
				std::bit_width(profile_max_hash);
			const std::uint64_t exact_profile_max_hash =
				(std::uint64_t{1u} << profile_hash_bits) - 1u;
			const std::span<const std::byte> direct_section =
				mapped_image.compact_h32_direct();
			const auto direct_header_receipt =
				mapped_image.compact_h32_direct_header_receipt();
			if (profile_hash_bits == 0u
				|| profile_hash_bits
					> (std::numeric_limits<std::uint8_t>::max)()
				|| exact_profile_max_hash != direct_profile->max_hash
				|| direct_section.empty()
				|| !direct_header_receipt) {
				fail("compact-H32 direct mapped section is absent or incompatible");
				return;
			}
			auto mapped_owner = mapped_image.borrow_mapping_token();
			const bioseq::indexing::CompactH32BoundedOpenOptions
				direct_open_options{
					.expected_identity = bioseq::indexing::DirectIndexIdentity{
						.generation_id = head.generation_id,
						.source_identity = head.source_identity,
						.compatibility_checksum =
							head.compatibility_checksum,
						.target_length = manifest.target_length,
					},
					.expected_max_occ_cap = static_cast<std::uint32_t>(
						direct_profile->shape_v5->max_occ_cap),
					.expected_geometry =
						bioseq::indexing::CompactH32Geometry{
							.hash_domain_bits = static_cast<std::uint8_t>(
								profile_hash_bits),
							.max_hash = profile_max_hash,
						},
					.expected_direct_header_crc64 = direct_header_receipt,
				};
			auto bounded_direct = bioseq::indexing::
				open_compact_h32_direct_index_bounded(
					std::move(mapped_owner), direct_section,
					direct_open_options);
			if (!bounded_direct
				|| bounded_direct->evidence().direct_section_bytes
					!= direct_section.size()
				|| bounded_direct->evidence().header_bytes_examined
					!= bioseq::indexing::
						kCompactH32DirectIndexHeaderBytes
				|| bounded_direct->evidence().payload_bytes_examined != 0u
				|| !bounded_direct->evidence().
					expected_direct_header_crc64_checked) {
				fail(fmt::format(
					"compact-H32 bounded open rejected direct_error={}",
					bounded_direct
						? 0u
						: static_cast<unsigned>(bounded_direct.error())));
				return;
			}
			// The nested fixed header and its outer CRC receipt are now
			// authenticated. Start fail-open metadata readahead now; selected
			// posting pages are advised by the existing descriptor-layout pass.
			// The unrelated outer MPF payload remains demand-paged.
			mapped_image.advise_compact_h32_direct_metadata_readahead(
				bounded_direct->view());
			const auto selected_page_adviser =
				mapped_image.borrow_compact_h32_selected_page_adviser(
					bounded_direct->view());
			auto owned_direct = bioseq::indexing::
				OwnedCompactH32DirectIndexView::bind(
					bounded_direct->image_owner(), bounded_direct->view());
			if (!owned_direct) {
				fail(fmt::format(
					"compact-H32 owner binding rejected wire_error={}",
					static_cast<unsigned>(owned_direct.error())));
				return;
			}
			auto made = ShapeReader::open_compact_direct(
				bioseq::parallelism::MemoryAdmissionController::instance(),
				head, manifest, selected_expected,
				static_cast<std::uint64_t>(group.max_occ),
				std::move(*owned_direct), selected_page_adviser);
			if (!made) {
				fail(fmt::format(
					"compact-H32 fragment reader rejected wire_error={}",
					static_cast<unsigned>(made.error())));
				return;
			}
			shape_reader.emplace(std::move(*made));
			read_stats = session.finish_mapped_compact_direct();
		} else {
			if (!authenticated_format5_directory) {
				fail("Format-5 control-scan route has no authenticated directory");
				return;
			}
			const std::span<const std::byte> mapped_payload =
				mapped_image.payload();
			auto mapped_payload_owner =
				mapped_image.borrow_mapping_token();
			auto made = ShapeReader::open(
				bioseq::parallelism::MemoryAdmissionController::instance(),
				head, manifest, selected_expected,
				*authenticated_format5_directory,
				static_cast<std::uint64_t>(group.max_occ),
				std::move(mapped_payload_owner),
				mapped_payload);
			if (!made) {
				fail(fmt::format(
					"fragment-native Format-5 reader rejected wire_error={}",
					static_cast<unsigned>(made.error())));
				return;
			}
			shape_reader.emplace(std::move(*made));
			read_stats = session.finish_mapped_format5_payload();
		}
		const std::uint64_t shape_v5_load_finished_ns =
			static_cast<std::uint64_t>(
				bioseq::instr::clock::now_ns());
		group.persisted_reader_load_host_wall_ns =
			shape_v5_load_finished_ns >= shape_v5_load_started_ns
				? std::max<std::uint64_t>(
					1u,
					shape_v5_load_finished_ns
						- shape_v5_load_started_ns)
				: 1u;
		const auto scheme =
			bioseq::indexing::minimizer_scheme_identity_for_backend(
				group.sketch_backend);
		const auto producer_backend =
			bioseq::indexing::minimizer_producer_backend_identity_for_backend(
				group.sketch_backend);
		const std::uint64_t expected_metadata_rows =
			2u + read_stats.directory_chunks_read;
		const std::uint64_t expected_metadata_bytes =
			MinimizerGenerationHead::encoded_bytes
			+ bioseq::indexing::minimizer_manifest_encoded_bytes(
				manifest.wire_format)
			+ read_stats.directory_bytes_read;
		const bool route_read_proof_matches =
			compact_h32_direct_requested
			? shape_reader->compact_direct_ready()
				&& shape_reader->compact_direct_only()
				&& shape_reader->mapped_format5_payload_bytes() == 0u
				&& shape_reader->format5_directory_entry_count() == 0u
				&& read_stats.directory_chunks_read == 0u
				&& read_stats.directory_bytes_read == 0u
			: !shape_reader->compact_direct_only()
				&& shape_reader->mapped_format5_payload_bytes()
					== manifest.payload_bytes
				&& read_stats.directory_chunks_read
					== manifest.directory_chunk_count
				&& read_stats.directory_bytes_read
					== manifest.directory_bytes;
		const bool read_proof_matches =
			shape_reader->ready()
			&& route_read_proof_matches
			&& shape_reader->target_length()
				== prepared_target.target_bp_size
			&& manifest.kmer_size == group.kmer_size
			&& manifest.window_size == group.window_size
			&& manifest.minimizer_scheme_id == scheme.id
			&& manifest.minimizer_scheme_version == scheme.version
			&& manifest.producer_backend_id == producer_backend.id
			&& manifest.producer_backend_identity_version
				== producer_backend.version
			&& manifest.shape_v5
			&& manifest.shape_v5->max_occ_cap
				== bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap
			&& head.generation_id != 0
			&& head.source_identity != 0
			&& read_stats.relation_opens == 1
			&& read_stats.head_scans == 1
			&& read_stats.head_rows_read == 1
			&& read_stats.manifest_directory_scans == 1
			&& read_stats.manifest_rows_read == 1
			&& read_stats.metadata_heap_rows_read == expected_metadata_rows
			&& read_stats.metadata_bytes_read == expected_metadata_bytes
			&& read_stats.total_heap_rows_read == expected_metadata_rows
			&& read_stats.total_bytes_read == expected_metadata_bytes
			&& read_stats.explicit_copy_bytes == expected_metadata_bytes
			&& read_stats.toast_values_read == 0
			&& read_stats.detoast_bytes == 0
			&& read_stats.metadata_repeated_reads == 0
			&& read_stats.full_generation.scan_calls == 0
			&& read_stats.bulk_read_calls == 0
			&& read_stats.range_scan_calls == 0
			&& read_stats.index_scans == 2
			&& read_stats.payload.delivered_chunks == 0
			&& read_stats.payload.heap_rows_read == 0
			&& read_stats.payload.delivered_bytes == 0
			&& read_stats.payload_bytes_read == 0
			&& read_stats.cold.route_empty == 0
			&& read_stats.cold.route_selective == 0
			&& read_stats.cold.route_full == 0
			&& read_stats.cold.head_scans == 1
			&& read_stats.cold.directory_scans == 1
			&& read_stats.source_parent_rows_read == 1
			&& read_stats.source_catalog_rows_read == 1
			&& read_stats.source_catalog_index_scans == 1
			&& read_stats.source_fragment_rows_read == 0
			&& read_stats.source_decoded_bp == 0
			&& source_counters.timed_equation_holds();
		if (!read_proof_matches) {
			fail("Format-5 READY identity/backend/scheme/read-work proof disagrees");
			return;
		}

		auto mapping_guard =
			std::move(mapped_image).release_mapping_lease_guard();
		if (!mapping_guard) {
			fail("Format-5 mapped reader lost its mmap/fd lifetime authority");
			return;
		}
		group.persisted_shape_v5_mapping_guard.emplace(
			std::move(mapping_guard));
		group.persisted_shape_v5_reader.emplace(
			std::move(*shape_reader));
		group.persisted_shape_v5_lookup_route =
			compact_h32_direct_requested
			? bioseq::indexing::FragmentAnchorLookupRoute::compact_h32_direct
			: bioseq::indexing::FragmentAnchorLookupRoute::format5_control_scans;
		group.target_index_source =
			::bioseq::pipeline_stats::TargetIndexSource::persisted;
		group.persisted_generation_id = head.generation_id;
		group.persisted_source_identity = head.source_identity;
		group.persisted_read_stats = read_stats;
		group.persisted_reader_loads = 1;
		if (!group.compact_direct_query_set_proof_pending) {
			group.build_set_checksum_present = true;
			group.build_set_checksum = group.key_set_checksum;
		} else if (!compact_h32_direct_requested
			|| group.key_set_checksum != 0u
			|| group.build_set_checksum_present
			|| group.build_set_checksum != 0u) {
			fail("compact-H32 deferred query-set proof crossed reader publication");
			return;
		}
		++prepared_target.physical_target_index_reads;
		return;
	}

	/* MPS Format-5 remains a sequence-wide persisted index.  Unlike the MPF
	 * route above, it has no authority to open the MPF-only mmap sidecar: read
	 * and authenticate the complete PostgreSQL payload into the established
	 * query-selected Shape-v5 reader. */
	if (full_mps_format5) {
		using ShapeReader = bioseq::indexing::PreparedShapeV5QueryReader;
		if (!authenticated_format5_directory) {
			fail("MPS Format-5 route has no authenticated directory");
			return;
		}
		const std::uint64_t shape_v5_load_started_ns =
			static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
		auto made = ShapeReader::begin(
			bioseq::parallelism::MemoryAdmissionController::instance(),
			head, manifest, selected_expected,
			*authenticated_format5_directory, group.query_hash_union,
			static_cast<std::uint64_t>(group.max_occ));
		if (!made) {
			fail(fmt::format(
				"MPS Format-5 reader begin rejected wire_error={}",
				static_cast<unsigned>(made.error())));
			return;
		}
		std::optional<ShapeReader> shape_reader{
			std::in_place, std::move(*made)};
		/* The reader owns the exact compact query-hash image from this point.
		 * Release the scheduler copy before allocating retained payload state. */
		if (!prepared_target.release_group_query_hash_union_noexcept(group)) {
			fail("MPS Format-5 canonical query-union release disagrees with admission ownership");
			return;
		}

		std::optional<MinimizerGenerationWireError> callback_error;
		std::exception_ptr callback_exception;
		const auto accept = [
				&callback_error,
				&callback_exception,
				&shape_reader](const bioseq::indexing::
				ValidatedMinimizerGenerationRowReceipt& receipt) noexcept {
			if (callback_error || callback_exception) return;
			try {
				auto accepted = shape_reader->accept(receipt);
				if (!accepted) callback_error = accepted.error();
			} catch (...) {
				callback_exception = std::current_exception();
			}
		};
		(void)session.read_full_generation_payload_once(accept, accept);
		if (callback_exception) {
			std::rethrow_exception(callback_exception);
		}
		if (callback_error) {
			fail(fmt::format(
				"MPS Format-5 payload ingest rejected wire_error={}",
				static_cast<unsigned>(*callback_error)));
			return;
		}
		if (auto finished = shape_reader->finish(); !finished) {
			fail(fmt::format(
				"MPS Format-5 reader final validation rejected wire_error={}",
				static_cast<unsigned>(finished.error())));
			return;
		}

		const MinimizerGenerationReadStats read_stats = session.finish();
		const std::uint64_t shape_v5_load_finished_ns =
			static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
		group.persisted_reader_load_host_wall_ns =
			shape_v5_load_finished_ns >= shape_v5_load_started_ns
				? std::max<std::uint64_t>(
					1u,
					shape_v5_load_finished_ns - shape_v5_load_started_ns)
				: 1u;
		const auto scheme =
			bioseq::indexing::minimizer_scheme_identity_for_backend(
				group.sketch_backend);
		const auto producer_backend =
			bioseq::indexing::minimizer_producer_backend_identity_for_backend(
				group.sketch_backend);
		const bool read_proof_matches =
			shape_reader->ready()
			&& shape_reader->target_length()
				== prepared_target.target_bp_size
			&& manifest.kmer_size == group.kmer_size
			&& manifest.window_size == group.window_size
			&& manifest.minimizer_scheme_id == scheme.id
			&& manifest.minimizer_scheme_version == scheme.version
			&& manifest.producer_backend_id == producer_backend.id
			&& manifest.producer_backend_identity_version
				== producer_backend.version
			&& manifest.shape_v5
			&& manifest.shape_v5->max_occ_cap
				== bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap
			&& head.generation_id != 0
			&& head.source_identity != 0
			&& read_stats.relation_opens == 1
			&& read_stats.head_scans == 1
			&& read_stats.manifest_directory_scans == 1
			&& read_stats.metadata_repeated_reads == 0
			&& read_stats.full_generation.scan_calls == 1
			&& read_stats.bulk_read_calls == 1
			&& read_stats.range_scan_calls == 1
			&& read_stats.index_scans == 3
			&& read_stats.payload.delivered_chunks
				== read_stats.payload.heap_rows_read
			&& read_stats.payload.delivered_bytes == manifest.payload_bytes
			&& read_stats.cold.route_empty == 0
			&& read_stats.cold.route_selective == 0
			&& read_stats.cold.route_full == 1
			&& read_stats.source_parent_rows_read == 1
			&& read_stats.source_catalog_rows_read == 1
			&& read_stats.source_catalog_index_scans == 1
			&& read_stats.source_fragment_rows_read == 0
			&& read_stats.source_decoded_bp == 0
			&& source_counters.timed_equation_holds();
		if (!read_proof_matches) {
			fail("MPS Format-5 READY identity/backend/scheme/read-work proof disagrees");
			return;
		}

		group.persisted_mps_shape_v5_reader.emplace(
			std::move(*shape_reader));
		group.target_index_source =
			::bioseq::pipeline_stats::TargetIndexSource::persisted;
		group.persisted_generation_id = head.generation_id;
		group.persisted_source_identity = head.source_identity;
		group.persisted_read_stats = read_stats;
		group.persisted_reader_loads = 1;
		group.build_set_checksum_present = true;
		group.build_set_checksum = group.key_set_checksum;
		++prepared_target.physical_target_index_reads;
		return;
	}

	// Format 4 still consumes the legacy u64 span. Keep its conversion out of
	// the Format-5 hot path: the latter now copies once directly from the
	// already sorted/unique MinimapSketchData union into its canonical owner.
	if (metadata == nullptr) {
		fail("grouped32 route has no authenticated directory metadata");
		return;
	}
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

	auto planned = bioseq::indexing::plan_minimizer_grouped32_cold_route(
		head, manifest, metadata->directory, query_hashes);
	if (!planned) {
		fail(fmt::format("grouped32 cold route rejected wire_error={}",
			static_cast<unsigned>(planned.error())));
		return;
	}
	Grouped32ColdPlan route_plan = std::move(*planned);
	session.bind_grouped32_cold_route_once(route_plan);

	using Grouped32Reader =
		bioseq::indexing::PreparedGrouped32QueryReader;
	std::optional<Grouped32Reader> reader;
	std::vector<Grouped32PayloadGroup> control_groups;
	std::vector<Grouped32PayloadGroup> position_groups;
	std::optional<MinimizerGenerationWireError> callback_error;
	std::exception_ptr callback_exception;
	const auto invoke_reader_callback =
		[&callback_error, &callback_exception](auto&& callback) noexcept {
			if (callback_error || callback_exception) return;
			try {
				auto result = callback();
				if (!result) callback_error = result.error();
			} catch (...) {
				callback_exception = std::current_exception();
			}
		};
	const auto callback_failed = [&]() noexcept {
		return callback_error.has_value()
			|| static_cast<bool>(callback_exception);
	};
	const auto fail_callback = [&](std::string_view phase) {
		if (callback_exception) {
			std::rethrow_exception(callback_exception);
		}
		fail(fmt::format("{} wire_error={}", phase,
			callback_error
				? static_cast<unsigned>(*callback_error)
				: 0u));
	};

	if (route_plan.route
			== bioseq::indexing::MinimizerGrouped32ColdLoadRoute::full) {
		const auto grouped32_validate = base_params.highway_kernels == nullptr
			? nullptr
			: base_params.highway_kernels->indexing
				.validate_non_scatter_local32_w2;
		auto made = Grouped32Reader::begin_full_generation_stream(
			bioseq::parallelism::MemoryAdmissionController::instance(),
			head, manifest, selected_expected, group.sketch_backend,
			metadata->directory.encoded_bytes(), query_hashes,
			static_cast<std::uint64_t>(group.max_occ), grouped32_validate);
		if (!made) {
			fail(fmt::format("grouped32 full reader begin rejected wire_error={}",
				static_cast<unsigned>(made.error())));
			return;
		}
		reader.emplace(std::move(*made));
		(void)session.read_full_generation_payload_once(
			[&](const bioseq::indexing::IndexChunkAddress& address,
				const char* bytes, std::size_t byte_count) {
				invoke_reader_callback([&] {
					return
					reader->accept_full_generation_control_chunk(
						address, std::as_bytes(
							std::span<const char>{bytes, byte_count}));
				});
			},
			[&](const bioseq::indexing::IndexChunkAddress& address,
				const char* bytes, std::size_t byte_count) {
				invoke_reader_callback([&] {
					return
					reader->accept_full_generation_position_chunk(
						address, std::as_bytes(
							std::span<const char>{bytes, byte_count}));
				});
			});
		if (callback_failed()) {
			fail_callback("grouped32 full payload ingest failed");
			return;
		}
		invoke_reader_callback(
			[&] { return reader->finish_full_generation_stream(); });
		if (callback_failed()) {
			fail_callback("grouped32 full final validation failed");
			return;
		}
	} else {
		const auto grouped32_validate = base_params.highway_kernels == nullptr
			? nullptr
			: base_params.highway_kernels->indexing
				.validate_non_scatter_local32_w2;
		auto made = Grouped32Reader::begin(
			bioseq::parallelism::MemoryAdmissionController::instance(),
			head, manifest, selected_expected, group.sketch_backend,
			metadata->directory.encoded_bytes(), query_hashes,
			static_cast<std::uint64_t>(group.max_occ), grouped32_validate);
		if (!made) {
			fail(fmt::format("grouped32 selective reader begin rejected wire_error={}",
				static_cast<unsigned>(made.error())));
			return;
		}
		reader.emplace(std::move(*made));
		if (!route_plan.candidate_authority_equals(
				reader->control_ordinals(),
				route_plan.control_requests,
				route_plan.position_upper_bound_requests)) {
			fail("grouped32 reader candidate authority disagrees with the cold route");
			return;
		}
		auto grouped_controls = group_grouped32_cold_requests(
			route_plan.control_requests);
		if (!grouped_controls) {
			fail(fmt::format("grouped32 control grouping rejected wire_error={}",
				static_cast<unsigned>(grouped_controls.error())));
			return;
		}
		control_groups = std::move(*grouped_controls);
		if (control_groups.size() != route_plan.control_groups
			|| !grouped32_group_authority_equals(
				control_groups, route_plan.control_requests)) {
			fail("grouped32 control groups do not exactly preserve planner authority");
			return;
		}
		session.plan_control_groups(control_groups);
		for (std::size_t group_index = 0;
			 group_index < control_groups.size(); ++group_index) {
			(void)session.read_chunks_in_range(
				group_index,
				bioseq::indexing::IndexChunkCachePolicy::bypass_one_shot,
				[&](const bioseq::indexing::IndexChunkAddress& address,
					const char* bytes, std::size_t byte_count) {
					invoke_reader_callback([&] {
						return reader->accept_control_chunk(
							address, std::as_bytes(
								std::span<const char>{bytes, byte_count}));
					});
				});
			if (callback_failed()) {
				fail_callback("grouped32 selective control ingest failed");
				return;
			}
		}
		invoke_reader_callback([&] { return reader->finish_controls(); });
		if (callback_failed()) {
			fail_callback("grouped32 selective control validation failed");
			return;
		}
		auto grouped_positions =
			group_retained_grouped32_position_requests(
				route_plan, reader->retained_position_ordinals());
		if (!grouped_positions) {
			fail(fmt::format("grouped32 position grouping rejected wire_error={}",
				static_cast<unsigned>(grouped_positions.error())));
			return;
		}
		position_groups = std::move(*grouped_positions);
		if (position_groups.size()
			> route_plan.position_groups_upper_bound) {
			fail("grouped32 retained position groups exceed the planner bound");
			return;
		}
		session.plan_position_groups(position_groups);
		for (std::size_t group_index = 0;
			 group_index < position_groups.size(); ++group_index) {
			(void)session.read_chunks_in_range(
				group_index,
				bioseq::indexing::IndexChunkCachePolicy::bypass_one_shot,
				[&](const bioseq::indexing::IndexChunkAddress& address,
					const char* bytes, std::size_t byte_count) {
					invoke_reader_callback([&] {
						return reader->accept_position_chunk(
							address, std::as_bytes(
								std::span<const char>{bytes, byte_count}));
					});
				});
			if (callback_failed()) {
				fail_callback("grouped32 selective position ingest failed");
				return;
			}
		}
		invoke_reader_callback([&] { return reader->finish_load(); });
		if (callback_failed()) {
			fail_callback("grouped32 selective final validation failed");
			return;
		}
	}
	const MinimizerGenerationReadStats read_stats = session.finish();
	const auto scheme =
		bioseq::indexing::minimizer_scheme_identity_for_backend(
			group.sketch_backend);
	const auto producer_backend =
		bioseq::indexing::minimizer_producer_backend_identity_for_backend(
			group.sketch_backend);
	const auto cold_sql_value = [](std::uint64_t value) noexcept {
		return std::min(
			value,
			bioseq::storage::indexing::ColdIndexPreparationStatsPg::
				sql_bigint_max);
	};
	const auto selected_route = route_plan.route;
	const bool empty_route = selected_route
		== bioseq::indexing::MinimizerGrouped32ColdLoadRoute::empty;
	const bool selective_route = selected_route
		== bioseq::indexing::MinimizerGrouped32ColdLoadRoute::selective;
	const bool full_route = selected_route
		== bioseq::indexing::MinimizerGrouped32ColdLoadRoute::full;
	const bool route_stats_match =
		read_stats.cold.route_empty == (empty_route ? 1u : 0u)
		&& read_stats.cold.route_selective == (selective_route ? 1u : 0u)
		&& read_stats.cold.route_full == (full_route ? 1u : 0u)
		&& read_stats.cold.route_candidate_rows
			== cold_sql_value(route_plan.candidate_rows_upper_bound)
		&& read_stats.cold.route_candidate_bytes
			== cold_sql_value(route_plan.candidate_bytes_upper_bound)
		&& read_stats.cold.route_full_rows
			== cold_sql_value(route_plan.full_payload_rows)
		&& read_stats.cold.route_full_bytes
			== cold_sql_value(route_plan.full_payload_bytes)
		&& read_stats.cold.route_selective_cost
			== cold_sql_value(route_plan.selective_cost)
		&& read_stats.cold.route_full_cost
			== cold_sql_value(route_plan.full_cost);
	const bool common_read_stats_match =
		read_stats.relation_opens == 1
		&& read_stats.head_scans == 1
		&& read_stats.manifest_directory_scans == 1
		&& read_stats.metadata_repeated_reads == 0
		&& read_stats.index_scans == 2u + read_stats.range_scan_calls
		&& read_stats.planned_request_groups
			== read_stats.executed_request_groups
		&& read_stats.executed_request_groups
			== read_stats.range_scan_calls
		&& read_stats.cache_lookups == 0
		&& read_stats.cache_populations == 0
		&& read_stats.cold.memory_reroutes == 0;
	const bool selected_read_stats_match = !full_route
		&& read_stats.full_generation.scan_calls == 0
		&& read_stats.bulk_read_calls == 2
		&& read_stats.controls.plan_calls == 1
		&& read_stats.positions.plan_calls == 1
		&& read_stats.controls.planned_request_groups
			== control_groups.size()
		&& read_stats.positions.planned_request_groups
			== position_groups.size()
		&& (!empty_route
			|| (read_stats.range_scan_calls == 0
				&& read_stats.payload_heap_rows_read == 0
				&& read_stats.payload_bytes_read == 0));
	const bool full_read_stats_match = !full_route
		|| (read_stats.full_generation.scan_calls == 1
			&& read_stats.bulk_read_calls == 1
			&& read_stats.range_scan_calls == 1
			&& read_stats.payload.delivered_chunks
				== route_plan.full_payload_rows
			&& read_stats.payload.delivered_bytes
				== route_plan.full_payload_bytes);
	if (!reader || !reader->ready()
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
		|| !route_stats_match
		|| !common_read_stats_match
		|| (!full_route && !selected_read_stats_match)
		|| !full_read_stats_match
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

void BatchProcessor::prepare_gpu_persisted_shape_v5_target(
		const Oid database_oid,
		const Oid tablespace_oid,
		const Oid main_relfilenode,
		const Oid user_relid,
		const std::int16_t column_attno,
		const std::string_view index_name,
		const std::uint64_t sequence_rowno,
		const std::uint64_t target_bp_size,
		const std::uint64_t query_count,
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const Minimap2Params& params,
		const bioseq::storage::indexing::BorrowedExecutorReadContextPg&
			executor_read,
		GpuPreparedShapeV5Target& prepared_target) {
	using namespace bioseq::storage::indexing;
	using bioseq::indexing::GpuPersistedIndexFoundationError;
	using bioseq::indexing::MinimizerGenerationWireFormat;
	const auto fail = [](const int sqlstate, std::string message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([sqlstate, &message] {
			ereport(ERROR,
				(errcode(sqlstate),
				 errmsg("required persisted GPU Format-5 target is unavailable"),
				 errdetail("%s", message.c_str()),
				 errhint("Publish a matching Format-5/ABI3 READY generation; "
					"GPU preparation has no CPU, SIMD, recompute, or legacy-index fallback.")));
		});
	};
	prepared_target = GpuPreparedShapeV5Target{};
	const MinimizerSketchBackend backend =
		pipe::target_index_sketch_backend_from_params(params);
	if (database_oid != MyDatabaseId
		|| !OidIsValid(user_relid)
		|| !OidIsValid(tablespace_oid)
		|| !OidIsValid(main_relfilenode)
		|| column_attno <= 0
		|| sequence_rowno
			> static_cast<std::uint64_t>(
				(std::numeric_limits<std::int64_t>::max)())
		|| target_bp_size == 0
		|| query_count == 0
		|| !pipe::persisted_shape_v5_gpu_preparation_tuple_valid(
			alphabet_code,
			pgseq_supported_alphabets::alphabet_count,
			storage_unit_byte_width,
			backend,
			params)
		|| !executor_read.matches(
			database_oid, tablespace_oid, main_relfilenode, user_relid)) {
		fail(ERRCODE_INVALID_PARAMETER_VALUE,
			"GPU prepared-target identity, pinned k/w, or all-GPU selector is invalid");
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
	if (source.alphabet_code != alphabet_code
		|| source.storage_unit_byte_width != storage_unit_byte_width
		|| source.locator.total_bp_size != target_bp_size) {
		fail(ERRCODE_DATA_CORRUPTED,
			"authoritative source identity disagrees with the BatchJoin target typmod or length");
		return;
	}
	identity.table_name = format_seqidx_name(
		index_name, database_oid, tablespace_oid, main_relfilenode,
		column_attno, source.locator.bucket_idx, source.locator.shard_idx);
	const auto expected =
		bioseq::indexing::expected_minimizer_compatibility_for_backend(
			MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
			backend, source.alphabet_code, source.storage_unit_byte_width,
			static_cast<std::uint16_t>(params.kmer_size),
			static_cast<std::uint16_t>(params.window_size),
			bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap);
	if (!expected) {
		fail(ERRCODE_DATA_CORRUPTED, fmt::format(
			"GPU Format-5 compatibility rejected wire_error={}",
			static_cast<unsigned>(expected.error())));
		return;
	}
	auto opened = open_exact_minimizer_generation_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read,
		*expected);
	if (!opened) {
		const int sqlstate = opened.failure.kind
			== MinimizerGenerationRelationKind::missing
			|| opened.failure.kind
				== MinimizerGenerationRelationKind::legacy_exact
			? ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
			: ERRCODE_DATA_CORRUPTED;
		fail(sqlstate, fmt::format(
			"generation relation {} rejected kind={} schema_error={}",
			identity.table_name,
			static_cast<unsigned>(opened.failure.kind),
			static_cast<unsigned>(opened.failure.revision3_error)));
		return;
	}
	IndexStorageFragmentGenerationReadSessionPg& session = *opened.session;
	const auto head = session.read_active_head_once();
	const auto& metadata = session.read_manifest_directory_once(head);
	const auto manifest = metadata.manifest;
	session.validate_source_attestation(std::move(source_proof));

	auto& runtime = bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
	std::uint64_t cold_load_identity = 14695981039346656037ull;
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, head.generation_id);
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, head.source_identity);
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, head.manifest_checksum);
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, manifest.directory_checksum);
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, manifest.payload_checksum);
	cold_load_identity = bioseq::common::hash_combine_64(
		cold_load_identity, sequence_rowno);
	for (const std::uint8_t byte : runtime.features().device_uuid) {
		cold_load_identity = bioseq::common::hash_combine_64(
			cold_load_identity, byte);
	}
	if (cold_load_identity == 0u) cold_load_identity = 1u;

	auto ingest = bioseq::indexing::GpuShapeV5FullGenerationIngest::begin(
		runtime, head, manifest, *expected, metadata.directory,
		cold_load_identity);
	if (!ingest) {
		fail(ERRCODE_FEATURE_NOT_SUPPORTED, fmt::format(
			"GPU Format-5 resident admission rejected foundation_error={}",
			static_cast<unsigned>(ingest.error())));
		return;
	}
	std::optional<GpuPersistedIndexFoundationError> callback_error;
	std::exception_ptr callback_exception;
	const auto accept = [&](const bioseq::indexing::
			ValidatedMinimizerGenerationRowReceipt& receipt) noexcept {
		if (callback_error || callback_exception) return;
		try {
			auto accepted = ingest->accept_validated_row(receipt);
			if (!accepted) callback_error = accepted.error();
		} catch (...) {
			callback_exception = std::current_exception();
		}
	};
	(void)session.read_full_generation_payload_once(accept, accept);
	if (callback_exception) std::rethrow_exception(callback_exception);
	if (callback_error) {
		fail(ERRCODE_DATA_CORRUPTED, fmt::format(
			"GPU Format-5 payload ingest rejected foundation_error={}",
			static_cast<unsigned>(*callback_error)));
		return;
	}
	auto finished = ingest->finish();
	if (!finished) {
		fail(ERRCODE_DATA_CORRUPTED, fmt::format(
			"GPU Format-5 resident publication rejected foundation_error={}",
			static_cast<unsigned>(finished.error())));
		return;
	}
	std::shared_ptr<const bioseq::indexing::GpuResidentShapeV5Target>
		resident = std::move(*finished);
	const MinimizerGenerationReadStats read_stats = session.finish();
	const auto& resident_identity = resident->identity();
	const bool proof_matches = resident->validated()
		&& resident_identity.generation_id == head.generation_id
		&& resident_identity.source_identity == head.source_identity
		&& resident_identity.source_content_checksum
			== head.source_content_checksum
		&& resident_identity.manifest_checksum == head.manifest_checksum
		&& resident_identity.directory_checksum
			== manifest.directory_checksum
		&& resident_identity.payload_checksum == manifest.payload_checksum
		&& resident_identity.cold_load_identity == cold_load_identity
		&& resident_identity.device_uuid == runtime.features().device_uuid
		&& manifest.target_length == target_bp_size
		&& manifest.alphabet_code == alphabet_code
		&& manifest.storage_unit_byte_width == storage_unit_byte_width
		&& manifest.gpu_abi_version
			== bioseq::indexing::kMinimizerShapeV5GpuAbiVersion
		&& manifest.shape_v5
		&& manifest.shape_v5->max_occ_cap
			== bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap
		&& read_stats.relation_opens == 1
		&& read_stats.head_scans == 1
		&& read_stats.manifest_directory_scans == 1
		&& read_stats.metadata_repeated_reads == 0
		&& read_stats.full_generation.scan_calls == 1
		&& read_stats.bulk_read_calls == 1
		&& read_stats.range_scan_calls == 1
		&& read_stats.index_scans == 3
		&& read_stats.payload.delivered_chunks
			== read_stats.payload.heap_rows_read
		&& read_stats.payload.delivered_bytes == manifest.payload_bytes
		&& read_stats.cold.route_empty == 0
		&& read_stats.cold.route_selective == 0
		&& read_stats.cold.route_full == 1
		&& read_stats.source_parent_rows_read == 1
		&& read_stats.source_catalog_rows_read == 1
		&& read_stats.source_catalog_index_scans == 1
		&& read_stats.source_fragment_rows_read == 0
		&& read_stats.source_decoded_bp == 0
		&& source_counters.timed_equation_holds();
	if (!proof_matches) {
		fail(ERRCODE_DATA_CORRUPTED,
			"GPU Format-5 READY identity, read-work, upload, or source proof disagrees");
		return;
	}

	prepared_target.target_rowno = static_cast<std::int64_t>(sequence_rowno);
	prepared_target.target_bp_size = target_bp_size;
	prepared_target.query_count = query_count;
	prepared_target.persisted_read_stats = read_stats;
	prepared_target.resident = std::move(resident);
	prepared_target.resident_view =
		bioseq::gpu::vulkan::PhaseCShapeV5ResidentView{
			.descriptors = &prepared_target.resident->descriptors(),
			.controls = &prepared_target.resident->controls(),
			.positions = &prepared_target.resident->positions(),
			.descriptor_bytes = prepared_target.resident->descriptor_bytes(),
			.control_bytes = prepared_target.resident->control_bytes(),
			.position_bytes = prepared_target.resident->position_bytes(),
			.position_count = prepared_target.resident->position_count(),
			.max_occ_cap =
				bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap,
			.generation_id = resident_identity.generation_id,
			.source_identity = resident_identity.source_identity,
			.cold_load_identity = resident_identity.cold_load_identity,
			.partition_count = prepared_target.resident->descriptor_count(),
			.abi_version = resident_identity.gpu_abi_version,
			.device_uuid = resident_identity.device_uuid,
		};
	if (!prepared_target.ready()) {
		fail(ERRCODE_DATA_CORRUPTED,
			"GPU Format-5 prepared-target view failed its final ownership check");
	}
}

} // namespace bioseq::simd
