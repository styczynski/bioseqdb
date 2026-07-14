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

export module bioseq.scheduler.dispatch_processor;

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
 * Execution proof is owned by the target-independent canonical value in
 * bioseq::pipeline_stats. Do not reintroduce a Highway-local stats owner.
 */
export enum class BatchInvocationRole : std::uint8_t {
    primary,
    extra_query,
};

export class BatchProcessor {
public:
	struct CpuPreparedIndexGroup {
		std::int32_t kmer_size = 0;
		std::int32_t window_size = 0;
		std::int32_t max_occ = 0;
		MinimizerSketchBackend sketch_backend =
			MinimizerSketchBackend::Scalar;
		std::vector<MinimapSketchData> query_hash_union{};
		/* Declared before target_index so destruction releases the owned
		 * index storage before returning its admission reservation. */
		std::optional<bioseq::parallelism::MemoryPermit> target_index_permit{};
		std::shared_ptr<bioseq::indexing::MinimizerHashmapFragmentIndex>
			target_index{};
		/* Revision-3 grouped32 is already a native MinimizerLookupIndex.  Keep
		 * the authenticated reader itself; ordinal 2 must never transcode its
		 * positions through the legacy pair/hashmap assembly path. */
		std::optional<bioseq::indexing::PreparedGrouped32QueryReader>
			persisted_grouped32_reader{};
		/* Production Format-5 is consumed in its native shape/SoA form.  Keep
		 * it distinct from the frozen Format-4 reader so a row can never be
		 * silently relabelled or transcoded through the legacy hashmap. */
		std::optional<bioseq::indexing::PreparedShapeV5QueryReader>
			persisted_shape_v5_reader{};
		::bioseq::pipeline_stats::TargetIndexSource target_index_source =
			::bioseq::pipeline_stats::TargetIndexSource::none;
		std::uint64_t key_set_checksum = 0;
		std::uint64_t target_index_builds = 0;
		std::uint64_t target_index_reuses = 0;
		std::uint64_t duplicate_build_attempts = 0;
		std::uint64_t persisted_reader_loads = 0;
		std::uint64_t persisted_reader_queries = 0;
		std::uint64_t persisted_generation_id = 0;
		std::uint64_t persisted_source_identity = 0;
		bioseq::storage::indexing::MinimizerGenerationReadStats
			persisted_read_stats{};
		bool build_set_checksum_present = false;
		std::uint64_t build_set_checksum = 0;
	};

	/* One object per physical target row.  It survives every 8192-query wave
	 * in TupleDispatchBuffer and may contain one parameter group per effective
	 * (k,w,max_occ) shape.  GPU locator execution never consumes this object. */
	struct CpuPreparedTarget {
		std::int64_t target_rowno = -1;
		std::uint64_t target_bp_size = 0;
		std::uint64_t query_count = 0;
		std::vector<CpuPreparedIndexGroup> groups{};
		std::uint64_t physical_target_index_reads = 0;
		std::uint64_t total_unique_keys = 0;
		std::uint64_t key_set_checksum = 0;
		bool unions_ready = false;
		bool persisted_read_stats_published = false;
		/* The aggregate permit belongs to the target-wide object, not to an
		 * individual parameter group.  It is deliberately declared after
		 * groups: default move-assignment replaces old union storage
		 * before releasing the old permit.  The explicit destructor enforces
		 * the corresponding deallocate-before-release order at destruction. */
		std::uint64_t query_union_admission_bytes = 0;
		std::optional<bioseq::parallelism::MemoryPermit> query_union_permit{};

		CpuPreparedTarget() = default;
		CpuPreparedTarget(const CpuPreparedTarget&) = delete;
		CpuPreparedTarget& operator=(const CpuPreparedTarget&) = delete;
		CpuPreparedTarget(CpuPreparedTarget&&) noexcept = default;
		CpuPreparedTarget& operator=(CpuPreparedTarget&&) noexcept = default;
		void release_query_union_state_noexcept() noexcept {
			std::vector<CpuPreparedIndexGroup>{}.swap(groups);
			query_union_admission_bytes = 0;
			query_union_permit.reset();
		}
		~CpuPreparedTarget() noexcept {
			release_query_union_state_noexcept();
		}

		[[nodiscard]] CpuPreparedIndexGroup* find_group(
				std::int32_t kmer_size,
				std::int32_t window_size,
				std::int32_t max_occ,
				MinimizerSketchBackend sketch_backend) noexcept;
	};

	/**
	 * Narrow SQL-test seam for the deallocate-before-permit reprepare contract.
	 * It deliberately populates one CpuPreparedTarget object twice instead of
	 * constructing a later scan object, so stale outer or nested capacity is
	 * observable.  No PostgreSQL state is touched.
	 */
	[[nodiscard]] static bool
	cpu_prepared_target_same_object_reprepare_probe() {
		bioseq::parallelism::MemoryAdmissionController admission;
		CpuPreparedTarget target;
		auto populate = [&](const std::size_t sketch_capacity) -> bool {
			target.release_query_union_state_noexcept();
			auto permit = admission.try_reserve(0u);
			if (!permit) return false;
			target.query_union_permit.emplace(std::move(*permit));

			const std::uint64_t outer_headroom =
				bioseq::parallelism::saturating_mul(
					sizeof(CpuPreparedIndexGroup),
					bioseq::parallelism::
						kVectorPreallocationHeadroomFactor);
			if (outer_headroom
					== (std::numeric_limits<std::uint64_t>::max)()
				|| !target.query_union_permit->grow_to(outer_headroom)) {
				return false;
			}
			target.groups.reserve(1u);
			const std::uint64_t outer_bytes =
				bioseq::parallelism::saturating_mul(
					target.groups.capacity(),
					sizeof(CpuPreparedIndexGroup));
			if (outer_bytes > outer_headroom) return false;
			target.query_union_admission_bytes = outer_bytes;
			target.query_union_permit->shrink_to(outer_bytes);
			target.groups.emplace_back();

			const std::uint64_t inner_headroom =
				bioseq::parallelism::saturating_mul(
					bioseq::parallelism::saturating_mul(
						sketch_capacity, sizeof(MinimapSketchData)),
					bioseq::parallelism::
						kVectorPreallocationHeadroomFactor);
			const std::uint64_t peak =
				bioseq::parallelism::saturating_add(
					outer_bytes, inner_headroom);
			if (inner_headroom
					== (std::numeric_limits<std::uint64_t>::max)()
				|| peak == (std::numeric_limits<std::uint64_t>::max)()
				|| !target.query_union_permit->grow_to(peak)) {
				return false;
			}
			target.groups.front().query_hash_union.reserve(sketch_capacity);
			const std::uint64_t inner_bytes =
				bioseq::parallelism::saturating_mul(
					target.groups.front().query_hash_union.capacity(),
					sizeof(MinimapSketchData));
			const std::uint64_t retained =
				bioseq::parallelism::saturating_add(
					outer_bytes, inner_bytes);
			if (inner_bytes > inner_headroom
				|| retained == (std::numeric_limits<std::uint64_t>::max)()) {
				return false;
			}
			target.query_union_admission_bytes = retained;
			target.query_union_permit->shrink_to(retained);
			return admission.reserved_bytes() == retained;
		};

		if (!populate(8193u)) return false;
		const std::size_t oversized_capacity =
			target.groups.front().query_hash_union.capacity();
		target.release_query_union_state_noexcept();
		if (!target.groups.empty() || target.groups.capacity() != 0u
			|| target.query_union_permit.has_value()
			|| target.query_union_admission_bytes != 0u
			|| admission.reserved_bytes() != 0u) {
			return false;
		}
		if (!populate(1u)) return false;
		const bool smaller_reprepare =
			target.groups.size() == 1u
			&& target.groups.front().query_hash_union.capacity()
				< oversized_capacity;
		target.release_query_union_state_noexcept();
		return smaller_reprepare
			&& target.groups.capacity() == 0u
			&& !target.query_union_permit.has_value()
			&& admission.reserved_bytes() == 0u;
	}

	[[nodiscard]] static Minimap2Params effective_params_for_pair(
		const Minimap2Params& base,
		std::size_t min_bp) noexcept;

	void prepare_cpu_shared_query_unions(
		std::uint8_t alphabet_code,
		std::uint8_t storage_unit_byte_width,
		std::uint64_t target_bp_size,
		std::span<const pgseq_t* const> query_headers,
		const Minimap2Params& base_params,
		bool include_persisted_parameter_group,
		CpuPreparedTarget& prepared_target);

	void prepare_cpu_persisted_grouped32_target(
		Oid database_oid,
		Oid tablespace_oid,
		Oid main_relfilenode,
		Oid user_relid,
			std::int16_t column_attno,
			std::string_view index_name,
			std::uint64_t sequence_rowno,
			const bioseq::storage::indexing::BorrowedExecutorReadContextPg&
				executor_read,
			CpuPreparedTarget& prepared_target);

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
        // Borrowed statement snapshot owned by the executor/admin caller.
        // scanner_pg validates that it is registered MVCC state and never
        // changes its registration count.
        Snapshot                                         snapshot             = nullptr;
        // Per-row optional locator. nullopt means "use the inline pgseq_t at
        // target_hdrs[i]" — scanner_pg is then constructed in in-memory mode.
        std::array<std::optional<bioseq::storage::seq_locator>,
            kDispatchBatchCapacity>                       target_locators{};
        // DOC0279 §11.3c — per-row columnar rowno (== index query_key) and the
        // user relation oid, so the stored per-fragment index can be assembled.
        std::array<std::int64_t, kDispatchBatchCapacity>  target_rownos{};
        std::array<std::uint64_t, kDispatchBatchCapacity> query_ordinals{};
        bool                                              explicit_query_ordinals = false;
        bool                                              shared_target_batch = false;
        bool                                              record_target_operational_outcomes = true;
		CpuPreparedTarget*                                cpu_prepared_target = nullptr;
        // Explicit per-row outcome written by the scanner helper. Match
        // production and index-source accounting must never infer one another.
		std::array<::bioseq::pipeline_stats::TargetIndexSource,
			kDispatchBatchCapacity>                       target_index_sources{};
		std::array<std::uint64_t, kDispatchBatchCapacity>
			persisted_generation_ids{};
		std::array<std::uint64_t, kDispatchBatchCapacity>
			persisted_source_identities{};
        Oid                                               user_relid           = InvalidOid;
        bioseq::indexing::IndexReadRequirement            index_read_requirement =
            bioseq::indexing::IndexReadRequirement::allow;
        bool                                              read_index_active    = false;
        bool                                              index_async_persist_active = false;
        bool                                              recompute_async_persist_active = false;
        // DOC0279 §11.3/§12.3 — the table's ACTIVE read index for the target
		// column. The source is retained separately so a configured catalog row
		// can never be laundered into an indistinguishable no-row default.
        std::string                                       active_index;
		bioseq::storage::indexing::ActiveReadPolicyResolutionSource
			active_index_policy_source = bioseq::storage::indexing::
				ActiveReadPolicyResolutionSource::unresolved;
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
        BatchInvocationRole invocation_role,
        std::uint64_t broadcast_query_ordinal,
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        ScannerPgContext* scanner_pg_ctx = nullptr);

};


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
