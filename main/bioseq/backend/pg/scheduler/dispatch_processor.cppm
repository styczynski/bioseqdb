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

#include "common.h"

#include <algorithm>
#include <array>
#include <bit>
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
import bioseq.common.radix_sort;
import bioseq.common.sequan_compat;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq::settings::index_run_buffer_bytes, bioseq_scheduler_*, bioseq::settings::index_memory_cap_mb GUCs
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.storage;
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
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;
import bioseq.scheduler.supervisor_pg;
namespace bioseq::simd {

export constexpr size_t kDispatchBatchCapacity = 8192;

/**
 * Existing pipeline evidence has a frozen u64 source field, while Format-6
 * authenticates a 128-bit source identity.  This domain-separated token is
 * diagnostic only: every Format-6 correctness decision retains and compares
 * all 128 authenticated bits.
 */
export [[nodiscard]] constexpr std::uint64_t
format6_source_diagnostic_token(
		const std::array<std::byte, 16>& source_identity) noexcept {
	std::array<std::byte, 32> bytes{
		std::byte{'B'}, std::byte{'S'}, std::byte{'E'}, std::byte{'Q'},
		std::byte{'F'}, std::byte{'6'}, std::byte{'D'}, std::byte{'I'},
		std::byte{'A'}, std::byte{'G'}, std::byte{'v'}, std::byte{'1'},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	for (std::size_t index = 0; index < source_identity.size(); ++index) {
		bytes[16u + index] = source_identity[index];
	}
	const std::uint64_t token =
		bioseq::indexing::crc64_ecma182(bytes);
	return token == 0u ? 0x9e3779b97f4a7c15ull : token;
}

namespace {
constexpr std::array<std::byte, 16> kFormat6DiagnosticIdentityA{
	std::byte{1}};
constexpr std::array<std::byte, 16> kFormat6DiagnosticIdentityB{
	std::byte{2}};
static_assert(format6_source_diagnostic_token(
	std::array<std::byte, 16>{}) != 0u);
static_assert(format6_source_diagnostic_token(kFormat6DiagnosticIdentityA)
	!= 0u);
static_assert(format6_source_diagnostic_token(kFormat6DiagnosticIdentityA)
	== format6_source_diagnostic_token(kFormat6DiagnosticIdentityA));
static_assert(format6_source_diagnostic_token(kFormat6DiagnosticIdentityA)
	!= format6_source_diagnostic_token(kFormat6DiagnosticIdentityB));
} // namespace
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
		enum class CpuQueryUnionStorage : std::uint8_t {
			minimizer_sketch,
			canonical_u32,
			canonical_u32_sequential_fallback,
			retained_sketch_only,
		};

	struct CpuPreparedQuerySketch {
		std::uint64_t query_ordinal = 0;
		std::uint64_t query_bp_size = 0;
		/* During parallel preparation each query owns an independent permit.
		 * The backend thread absorbs it into the target aggregate before
		 * publication.  It is declared before minimizers so exceptional
		 * teardown deallocates vector storage before releasing admission. */
		std::optional<bioseq::parallelism::MemoryPermit>
			parallel_preparation_permit{};
		std::vector<MinimapSketchData> minimizers{};
		pipe::MinimizerRangeExecutionProof proof{};
	};

	struct CpuPreparedIndexGroup {
		std::int32_t kmer_size = 0;
		std::int32_t window_size = 0;
		std::int32_t max_occ = 0;
		MinimizerSketchBackend sketch_backend =
			MinimizerSketchBackend::Scalar;
		std::vector<MinimapSketchData> query_hash_union{};
		/* Format-6 borrows this exact sorted-unique canonical image for its
		 * complete lifetime.  It is mutually exclusive with query_hash_union:
		 * publication is authenticated before the scheduler chooses which
		 * representation to construct, so there is never a second Q-sized
		 * conversion owner or sort.  Declaring it before the reader makes
		 * reverse destruction release the borrowing reader first. */
		std::vector<std::uint32_t> canonical_query_hash_union_u32{};
		/* Format-6 preparation may retain the exact per-query sketch produced
		 * while filling canonical_query_hash_union_u32.  Missing ordinals are
		 * intentional admission fallbacks and are sketched by ensure_q in the
		 * typed row path. */
		std::vector<CpuPreparedQuerySketch> retained_query_sketches{};
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
		/* Production MPF Format-5 keeps every physical-fragment index
		 * independent. Its only target-wide state is query-specific global
		 * occurrence counts required by max-occ pruning; no posting owner,
		 * span federation, or hash-to-fragment lookup index is retained. */
		/* Declared before the borrowing reader so reverse destruction drops
		 * every mapped span consumer before the backend-thread guard promptly
		 * forgets, unmaps and closes its PostgreSQL ResourceOwner lease. */
		std::optional<bioseq::storage::indexing::
			MappedMinimizerFormat5LeaseGuardPg>
			persisted_shape_v5_mapping_guard{};
		std::optional<
			bioseq::indexing::PreparedShapeV5FragmentQueryReader>
			persisted_shape_v5_reader{};
		/* Captured once when the persisted reader is opened.  Production call
		 * sites must pass this explicitly: compact_h32_direct is fail-closed and
		 * must never silently take the default Format-5 control-scan route. */
		bioseq::indexing::FragmentAnchorLookupRoute
			persisted_shape_v5_lookup_route = bioseq::indexing::
				FragmentAnchorLookupRoute::format5_control_scans;
		/* MPS Format-5 retains its sequence-wide query-selected image.  It is
		 * deliberately separate from the MPF mmap reader above: MPS consumes
		 * the authenticated PostgreSQL payload scan and must never borrow the
		 * MPF-only sidecar or fragment-native ownership proof. */
		std::optional<bioseq::indexing::PreparedShapeV5QueryReader>
			persisted_mps_shape_v5_reader{};
		std::optional<bioseq::indexing::MinimizerFormat6SelectiveReader>
			persisted_format6_reader{};
		::bioseq::pipeline_stats::TargetIndexSource target_index_source =
			::bioseq::pipeline_stats::TargetIndexSource::none;
		std::uint64_t key_set_checksum = 0;
		std::uint64_t target_index_builds = 0;
		std::uint64_t target_index_reuses = 0;
		std::uint64_t duplicate_build_attempts = 0;
		std::uint64_t persisted_reader_loads = 0;
		std::uint64_t persisted_reader_queries = 0;
		std::uint64_t persisted_reader_load_host_wall_ns = 0;
		std::uint64_t persisted_generation_id = 0;
		/* Correctness authority for Format-6.  The legacy u64 field below is
		 * retained only for the existing pipeline evidence ABI and must never
		 * be used to authenticate a Format-6 source. */
		std::array<std::byte, 16> persisted_format6_source_identity{};
		bool persisted_format6_source_identity_present = false;
		std::uint64_t persisted_source_identity = 0;
		bioseq::storage::indexing::MinimizerGenerationReadStats
			persisted_read_stats{};
			bool build_set_checksum_present = false;
			std::uint64_t build_set_checksum = 0;
			/* Compact-direct full-batch preparation already constructs the exact
			 * sorted unique query set needed by the reader.  While this is true the
			 * historical prepared/build checksum is deliberately unpublished; the
			 * backend thread must resolve it exactly once before worker fanout or
			 * materialize the admitted slow fallback before row execution. */
			bool compact_direct_query_set_proof_pending = false;

		[[nodiscard]] const CpuPreparedQuerySketch*
		find_retained_query_sketch(
				const std::uint64_t query_ordinal) const noexcept {
			const auto found = std::lower_bound(
				retained_query_sketches.begin(),
				retained_query_sketches.end(),
				query_ordinal,
				[](const CpuPreparedQuerySketch& sketch,
				   const std::uint64_t ordinal) noexcept {
					return sketch.query_ordinal < ordinal;
				});
			return found != retained_query_sketches.end()
					&& found->query_ordinal == query_ordinal
				? &*found
				: nullptr;
		}
	};

	/* One object per physical target row.  It survives every 8192-query wave
	 * in TupleDispatchBuffer and may contain one parameter group per effective
	 * (k,w,max_occ) shape.  GPU locator execution never consumes this object. */
	struct CpuPreparedTarget {
		std::int64_t target_rowno = -1;
		std::uint64_t target_bp_size = 0;
		std::uint64_t query_count = 0;
		/* CPU preparation runs beneath the same public Highway selection
		 * boundary as ordinary batch execution.  The selected table has static
		 * storage duration; retaining its neutral pointer and exact plan here
		 * lets every later query wave prove that it is reusing, rather than
		 * reconstructing, that target binding. */
		const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels =
			nullptr;
		::bioseq::simd_dispatch::HighwayDispatchInfo highway_dispatch{};
		std::int64_t highway_executed_target = 0;
		std::vector<CpuPreparedIndexGroup> groups{};
		std::uint64_t physical_target_index_reads = 0;
		std::uint64_t total_unique_keys = 0;
		std::uint64_t key_set_checksum = 0;
		std::uint64_t query_union_prepare_host_wall_ns = 0;
		bool unions_ready = false;
		bool query_union_prepare_host_wall_published = false;
		bool persisted_read_stats_published = false;
		/* The aggregate permit belongs to the target-wide object, not to an
		 * individual parameter group.  It is deliberately declared after
		 * groups: default move-assignment replaces old union storage
		 * before releasing the old permit.  The explicit destructor enforces
		 * the corresponding deallocate-before-release order at destruction. */
		std::uint64_t query_union_admission_bytes = 0;
		std::optional<bioseq::parallelism::MemoryPermit> query_union_permit{};
		std::uint64_t query_sketch_cache_admission_bytes = 0;
		std::optional<bioseq::parallelism::MemoryPermit>
			query_sketch_cache_permit{};

		CpuPreparedTarget() = default;
		CpuPreparedTarget(const CpuPreparedTarget&) = delete;
		CpuPreparedTarget& operator=(const CpuPreparedTarget&) = delete;
		CpuPreparedTarget(CpuPreparedTarget&&) noexcept = default;
		CpuPreparedTarget& operator=(CpuPreparedTarget&&) noexcept = default;

		[[nodiscard]] static constexpr bool same_highway_dispatch_identity(
				const ::bioseq::simd_dispatch::HighwayDispatchInfo& lhs,
				const std::int64_t lhs_target,
				const ::bioseq::simd_dispatch::HighwayDispatchInfo& rhs,
				const std::int64_t rhs_target) noexcept {
			return lhs.compiled_targets == rhs.compiled_targets
				&& lhs.cpu_supported_targets == rhs.cpu_supported_targets
				&& lhs.runnable_targets == rhs.runnable_targets
				&& lhs.planned_target == rhs.planned_target
				&& lhs_target == rhs_target;
		}

		[[nodiscard]] bool highway_binding_matches(
				const Minimap2Params& params) const noexcept {
			return highway_kernels != nullptr
				&& highway_kernels == params.highway_kernels
				&& same_highway_dispatch_identity(
					highway_dispatch,
					highway_executed_target,
					params.highway_dispatch,
					params.highway_executed_target);
		}

		[[nodiscard]] bool bind_highway_dispatch_once(
				const Minimap2Params& params) noexcept {
			const auto& plan = params.highway_dispatch;
			if (params.highway_kernels == nullptr
				|| plan.compiled_targets == 0
				|| plan.cpu_supported_targets == 0
				|| plan.runnable_targets
					!= (plan.compiled_targets & plan.cpu_supported_targets)
				|| params.highway_executed_target <= 0
				|| !std::has_single_bit(static_cast<std::uint64_t>(
					params.highway_executed_target))
				|| (plan.runnable_targets & params.highway_executed_target) == 0
				|| plan.planned_target != params.highway_executed_target) {
				return false;
			}
			const bool empty = highway_kernels == nullptr
				&& highway_dispatch.compiled_targets == 0
				&& highway_dispatch.cpu_supported_targets == 0
				&& highway_dispatch.runnable_targets == 0
				&& highway_dispatch.planned_target == 0
				&& highway_executed_target == 0;
			if (!empty) return highway_binding_matches(params);
			highway_kernels = params.highway_kernels;
			highway_dispatch = params.highway_dispatch;
			highway_executed_target = params.highway_executed_target;
			return true;
		}

		void release_query_union_state_noexcept() noexcept {
			std::vector<CpuPreparedIndexGroup>{}.swap(groups);
			query_union_admission_bytes = 0;
			query_union_permit.reset();
			query_sketch_cache_admission_bytes = 0;
			query_sketch_cache_permit.reset();
		}
		[[nodiscard]] bool release_group_query_hash_union_noexcept(
				CpuPreparedIndexGroup& group) noexcept {
			const std::uint64_t released_bytes =
				bioseq::parallelism::saturating_mul(
					group.query_hash_union.capacity(),
					static_cast<std::uint64_t>(
						sizeof(MinimapSketchData)));
			if (released_bytes
					== (std::numeric_limits<std::uint64_t>::max)()
				|| !query_union_permit.has_value()
				|| query_union_admission_bytes < released_bytes
				|| !group.canonical_query_hash_union_u32.empty()
				|| group.canonical_query_hash_union_u32.capacity() != 0u) {
				return false;
			}
			// The native persisted reader owns its canonical hash image now.
			// Deallocate the target-wide sketch union before shrinking the
			// aggregate permit so memory authority never trails ownership.
			std::vector<MinimapSketchData>{}.swap(group.query_hash_union);
			query_union_admission_bytes -= released_bytes;
			query_union_permit->shrink_to(query_union_admission_bytes);
			return true;
		}
		~CpuPreparedTarget() noexcept {
			release_query_union_state_noexcept();
		}

		[[nodiscard]] CpuPreparedIndexGroup* find_group(
			std::int32_t kmer_size,
			std::int32_t window_size,
			std::int32_t max_occ,
			MinimizerSketchBackend sketch_backend) noexcept;

		[[nodiscard]] bool query_set_proof_ready() const noexcept {
			return std::ranges::none_of(
				groups,
				[](const CpuPreparedIndexGroup& group) noexcept {
					return group.compact_direct_query_set_proof_pending;
				});
		}
	};
	static_assert(CpuPreparedTarget::same_highway_dispatch_identity(
		::bioseq::simd_dispatch::HighwayDispatchInfo{3, 3, 3, 1},
		1,
		::bioseq::simd_dispatch::HighwayDispatchInfo{3, 3, 3, 1},
		1));
	static_assert(!CpuPreparedTarget::same_highway_dispatch_identity(
		::bioseq::simd_dispatch::HighwayDispatchInfo{3, 3, 3, 1},
		1,
		::bioseq::simd_dispatch::HighwayDispatchInfo{3, 3, 3, 2},
		2));

	/* One restart-cold, PostgreSQL-authenticated Format-5 generation retained
	 * for the complete BatchJoin target lifecycle.  The resident owns the three
	 * Vulkan buffers; resident_view contains only stable pointers into that
	 * shared object and is never a publication/cache authority of its own. */
	struct GpuPreparedShapeV5Target {
		std::int64_t target_rowno = -1;
		std::uint64_t target_bp_size = 0;
		std::uint64_t query_count = 0;
		std::shared_ptr<const bioseq::indexing::GpuResidentShapeV5Target>
			resident{};
		bioseq::gpu::vulkan::PhaseCShapeV5ResidentView resident_view{};
		bioseq::storage::indexing::MinimizerGenerationReadStats
			persisted_read_stats{};
		std::uint64_t dispatched_query_count = 0;
		std::uint64_t dispatch_waves = 0;
		bool persisted_read_stats_published = false;

		GpuPreparedShapeV5Target() = default;
		GpuPreparedShapeV5Target(const GpuPreparedShapeV5Target&) = delete;
		GpuPreparedShapeV5Target& operator=(
			const GpuPreparedShapeV5Target&) = delete;
		GpuPreparedShapeV5Target(GpuPreparedShapeV5Target&&) noexcept = default;
		GpuPreparedShapeV5Target& operator=(
			GpuPreparedShapeV5Target&&) noexcept = default;

		[[nodiscard]] bool ready() const noexcept {
			return target_rowno >= 0
				&& target_bp_size != 0
				&& resident != nullptr
				&& resident->validated()
				&& resident_view.descriptors == &resident->descriptors()
				&& resident_view.controls == &resident->controls()
				&& resident_view.positions == &resident->positions()
				&& resident_view.descriptor_bytes
					== resident->descriptor_bytes()
				&& resident_view.control_bytes == resident->control_bytes()
				&& resident_view.position_bytes == resident->position_bytes()
				&& resident_view.position_count == resident->position_count()
				&& resident_view.max_occ_cap
					== bioseq::indexing::kMinimizerShapeV5DefaultMaxOccCap
				&& resident_view.partition_count == resident->descriptor_count()
				&& resident_view.abi_version
					== resident->identity().gpu_abi_version
				&& resident_view.device_uuid
					== resident->identity().device_uuid
				&& resident_view.generation_id
					== resident->identity().generation_id
				&& resident_view.source_identity
					== resident->identity().source_identity
				&& resident_view.cold_load_identity
					== resident->identity().cold_load_identity;
		}
	};

	/**
	 * Narrow SQL-test seam for the deallocate-before-permit reprepare contract.
	 * It deliberately populates one CpuPreparedTarget object twice instead of
	 * constructing a later scan object, so stale outer or nested capacity is
	 * observable.  No PostgreSQL state is touched.
	 */
	[[nodiscard]] static bool
	cpu_prepared_target_same_object_reprepare_probe() {
		/* The same existing SQL probe also guards the Format-6 borrowed-view
		 * lifetime: moving the target must transfer the group allocation
		 * without moving or duplicating its canonical u32 image. */
		{
			CpuPreparedTarget source;
			source.groups.emplace_back();
			auto& canonical =
				source.groups.front().canonical_query_hash_union_u32;
			canonical = {3u, 7u, 11u};
			const std::uint32_t* const borrowed = canonical.data();
			const bioseq::indexing::CanonicalQueryHashView view{
				.values32 = std::span<const std::uint32_t>{canonical},
				.width_bytes = sizeof(std::uint32_t),
				.storage_width_bytes = sizeof(std::uint32_t),
			};
			CpuPreparedTarget moved{std::move(source)};
			if (moved.groups.size() != 1u
				|| !moved.groups.front().query_hash_union.empty()
				|| moved.groups.front().query_hash_union.capacity() != 0u
				|| moved.groups.front().
					canonical_query_hash_union_u32.data() != borrowed
				|| view.size() != 3u
				|| view.at(0u) != 3u
				|| view.at(1u) != 7u
				|| view.at(2u) != 11u) {
				return false;
			}
		}
		/* The retained-sketch receipt is fail-closed, one-shot, and leaves no
		 * canonical owner behind.  Exercise both direct publication and the
		 * admitted historical sort/dedup fallback through the existing SQL seam. */
		{
			CpuPreparedTarget proof_target;
			proof_target.unions_ready = true;
			proof_target.query_count = 0u;
			proof_target.key_set_checksum =
				bioseq::indexing::kCompactDirectQuerySetFnvOffset;
			proof_target.groups.emplace_back();
			auto& group = proof_target.groups.front();
			group.kmer_size = 15;
			group.window_size = 10;
			group.max_occ = 500;
			group.sketch_backend = MinimizerSketchBackend::Scalar;
			group.compact_direct_query_set_proof_pending = true;
			const bioseq::indexing::CompactDirectQuerySetProof proof{
				.query_count = 0u,
				.query_occurrence_count = 0u,
				.unique_hash_count = 0u,
				.group_key_set_checksum = bioseq::indexing::
					compact_direct_query_set_checksum_prefix(15u, 10u, 500u),
				.present = true,
			};
			auto rejected = proof;
			rejected.query_count = 1u;
			if (proof_target.query_set_proof_ready()
				|| group.canonical_query_hash_union_u32.capacity() != 0u
				|| publish_compact_direct_query_set_proof(
					proof_target, group, rejected)
				|| !group.compact_direct_query_set_proof_pending
				|| !publish_compact_direct_query_set_proof(
					proof_target, group, proof)
				|| publish_compact_direct_query_set_proof(
					proof_target, group, proof)
				|| !proof_target.query_set_proof_ready()
				|| group.canonical_query_hash_union_u32.capacity() != 0u
				|| proof_target.total_unique_keys != 0u
				|| group.key_set_checksum != proof.group_key_set_checksum
				|| !group.build_set_checksum_present
				|| group.build_set_checksum != group.key_set_checksum) {
				return false;
			}

			CpuPreparedTarget fallback_target;
			fallback_target.unions_ready = true;
			fallback_target.query_count = 0u;
			fallback_target.key_set_checksum =
				bioseq::indexing::kCompactDirectQuerySetFnvOffset;
			fallback_target.groups.emplace_back();
			auto& fallback_group = fallback_target.groups.front();
			fallback_group.kmer_size = 15;
			fallback_group.window_size = 10;
			fallback_group.max_occ = 500;
			fallback_group.sketch_backend = MinimizerSketchBackend::Scalar;
			fallback_group.compact_direct_query_set_proof_pending = true;
			try {
				finalize_pending_compact_direct_query_set_proof_fallback(
					fallback_target);
			} catch (...) {
				return false;
			}
			if (!fallback_target.query_set_proof_ready()
				|| fallback_group.canonical_query_hash_union_u32.capacity()
					!= 0u
				|| fallback_target.total_unique_keys != 0u
				|| fallback_group.key_set_checksum
					!= proof.group_key_set_checksum
				|| fallback_target.key_set_checksum
					!= proof_target.key_set_checksum) {
				return false;
			}
		}
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
		const std::uint64_t retained_outer_bytes =
			bioseq::parallelism::saturating_mul(
				target.groups.capacity(),
				static_cast<std::uint64_t>(
					sizeof(CpuPreparedIndexGroup)));
		if (!target.release_group_query_hash_union_noexcept(
				target.groups.front())
			|| !target.groups.front().query_hash_union.empty()
			|| target.groups.front().query_hash_union.capacity() != 0u
			|| target.query_union_admission_bytes != retained_outer_bytes
			|| target.query_union_permit->bytes() != retained_outer_bytes
			|| admission.reserved_bytes() != retained_outer_bytes) {
			return false;
		}
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

	[[nodiscard]] static constexpr bool
	exactly_one_native_persisted_reader(
			const bool grouped32,
			const bool shape_v5,
			const bool mps_shape_v5,
			const bool format6) noexcept {
		return static_cast<unsigned>(grouped32)
			+ static_cast<unsigned>(shape_v5)
			+ static_cast<unsigned>(mps_shape_v5)
			+ static_cast<unsigned>(format6) == 1u;
	}

	[[nodiscard]] static constexpr bool
	format6_f5_fallback_allowed(
			const bool head_absent,
			const bool profile_applicable,
			const bool stream_io_started) noexcept {
		return !stream_io_started
			&& (head_absent || !profile_applicable);
	}

	[[nodiscard]] static Minimap2Params effective_params_for_pair(
		const Minimap2Params& base,
		std::size_t min_bp) noexcept;

	void prepare_cpu_shared_query_unions(
		std::uint8_t alphabet_code,
		std::uint8_t storage_unit_byte_width,
		std::uint64_t target_bp_size,
		std::span<const bioseq::pg_types::seq* const> query_headers,
		std::span<const Datum> query_recipe_datums,
		std::span<const pg_sequence_descriptor> query_recipe_descriptors,
		pg_sequence_read_session* query_read_session,
		const bioseq::storage::sequence_payload_allocation_observer*
			query_payload_observer,
		std::span<const std::uint64_t> query_ordinals,
		const Minimap2Params& base_params,
		bool include_persisted_parameter_group,
		CpuQueryUnionStorage union_storage,
		CpuPreparedTarget& prepared_target);

		void prepare_cpu_persisted_native_target(
		Oid database_oid,
		Oid tablespace_oid,
		Oid main_relfilenode,
		Oid user_relid,
		std::int16_t column_attno,
		std::string_view index_name,
		std::uint64_t sequence_rowno,
		std::uint64_t target_bp_size,
		std::uint8_t alphabet_code,
		std::uint8_t storage_unit_byte_width,
		std::span<const bioseq::pg_types::seq* const> query_headers,
		std::span<const Datum> query_recipe_datums,
		std::span<const pg_sequence_descriptor> query_recipe_descriptors,
		pg_sequence_read_session* query_read_session,
		const bioseq::storage::sequence_payload_allocation_observer*
			query_payload_observer,
		std::span<const std::uint64_t> query_ordinals,
		const Minimap2Params& base_params,
		const bioseq::storage::indexing::BorrowedExecutorReadContextPg&
			executor_read,
			CpuPreparedTarget& prepared_target);

		[[nodiscard]] static bool publish_compact_direct_query_set_proof(
			CpuPreparedTarget& prepared_target,
			CpuPreparedIndexGroup& group,
			const bioseq::indexing::CompactDirectQuerySetProof& proof) noexcept;

		static void finalize_pending_compact_direct_query_set_proof_fallback(
			CpuPreparedTarget& prepared_target);

	void prepare_gpu_persisted_shape_v5_target(
		Oid database_oid,
		Oid tablespace_oid,
		Oid main_relfilenode,
		Oid user_relid,
		std::int16_t column_attno,
		std::string_view index_name,
		std::uint64_t sequence_rowno,
		std::uint64_t target_bp_size,
		std::uint64_t query_count,
		std::uint8_t alphabet_code,
		std::uint8_t storage_unit_byte_width,
		const Minimap2Params& params,
		const bioseq::storage::indexing::BorrowedExecutorReadContextPg&
			executor_read,
		GpuPreparedShapeV5Target& prepared_target);

    /*
     * DOC0264 / DOC0274 — context required to wire the PostgreSQL facade
     * adapter into per-row pipeline dispatch. When non-null, stub-backed
     * targets stay source-backed while query literals use resident facades.
     * When null, the loop uses the Phase B in-memory baseline.
     */
	struct ScannerPgContext {
		/* Optional synchronous, one-shot cold preparation.  process_sequences
		 * consumes this record immediately after selecting dispatched_params and
		 * before entering profiling or typed execution.  It is deliberately a
		 * neutral callback: only the scheduler root may install it, and the
		 * callback receives the already-selected immutable table. */
		struct HighwayPreparationHook final {
			using Function = void (*)(
				void*, ScannerPgContext&, const Minimap2Params&);

			void* state = nullptr;
			Function function = nullptr;

			[[nodiscard]] constexpr bool empty() const noexcept {
				return state == nullptr && function == nullptr;
			}

			[[nodiscard]] constexpr bool valid() const noexcept {
				return state != nullptr && function != nullptr;
			}
		};

		HighwayPreparationHook highway_preparation{};
		/* Optional statement-root async ownership observer.  It is generic at
         * the pipeline boundary; the PostgreSQL trace owner supplies the
         * concrete enrollment/state-machine adapter. */
        bioseq::parallelism::tracked_job_observer async_job_observer{};
        /* Stable for the complete process_sequences call. Views copy only a
         * pointer to these records; every tracked task joins/drains before
         * ScannerPgContext leaves its owning dispatch frame. */
        bioseq::storage::sequence_payload_allocation_observer
            mmsearch_target_payload_observer{};
        bioseq::storage::sequence_payload_allocation_observer
            mmsearch_query_payload_observer{};
        Oid                                              db_oid               = InvalidOid;
        Oid                                              tablespace_oid       = InvalidOid;
        Oid                                              main_relfilenode     = InvalidOid;
        std::int16_t                                     mmsearch_seq_attno   = 0;
        std::int16_t                                     mmsearch_query_attno = 0;
        std::uint8_t                                     query_alphabet_code  = 0;
        std::uint8_t                                     query_storage_unit_byte_width = 0;
        bioseq::storage::seq_caches*                     caches               = nullptr;
        MemoryContext                                    parent_mctx          = nullptr;
        /* Wave-owned and borrowed. The dispatch buffer closes it only after
         * process_sequences and every synchronous GPU fence have returned. */
        pg_sequence_read_session*                        read_session         = nullptr;
        // Borrowed statement snapshot owned by the executor/admin caller.
        // The facade adapter validates registered MVCC state and never
        // changes its registration count.
        Snapshot                                         snapshot             = nullptr;
        // Borrowed from the BioseqScan that owns this dispatch. Locator-backed
        // targets must be promoted through this exact statement provider;
        // ScannerPgContext never closes or retains it independently.
        bioseq::storage::pg_sequence_source_provider*    source_provider      = nullptr;
        // Borrowed statement-root sink. It is required only by the accepted
        // persisted Shape-V5 Vulkan route and never owns through hot stats.
        ::bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger*
            persisted_gpu_operation_evidence_sink = nullptr;
        // Per-row optional locator. nullopt means "use the inline bioseq::pg_types::seq at
        // target_hdrs[i]" — the target then uses an in-memory facade.
        std::array<std::optional<bioseq::storage::seq_locator>,
            kDispatchBatchCapacity>                       target_locators{};
        // Same-relation per-row query locators. These use the target scan's
        // exact provider but retain their own physical attno and storage width.
        std::array<std::optional<bioseq::storage::seq_locator>,
            kDispatchBatchCapacity>                       query_locators{};
        /* Borrowed, batch-arena-owned expanded wrappers captured by
         * TupleDispatchBuffer.  A nonzero Datum is always paired with its
         * validated descriptor.  Typed dispatch adapts these through the same
         * alphabet-only basic_lazy_sequence_view ABI as locator and flat
         * inputs; it never owns or flattens the wrappers on CPU/SIMD paths. */
        std::array<Datum, kDispatchBatchCapacity>          target_recipe_datums{};
        std::array<Datum, kDispatchBatchCapacity>          query_recipe_datums{};
        std::array<pg_sequence_descriptor,
            kDispatchBatchCapacity>                       target_recipe_descriptors{};
        std::array<pg_sequence_descriptor,
            kDispatchBatchCapacity>                       query_recipe_descriptors{};
        // DOC0279 §11.3c — per-row columnar rowno (== index query_key) and the
        // user relation oid, so the stored per-fragment index can be assembled.
        std::array<std::int64_t, kDispatchBatchCapacity>  target_rownos{};
        std::array<std::uint64_t, kDispatchBatchCapacity> query_ordinals{};
        bool                                              explicit_query_ordinals = false;
        bool                                              shared_target_batch = false;
        bool                                              record_target_operational_outcomes = true;
		CpuPreparedTarget*                                cpu_prepared_target = nullptr;
		GpuPreparedShapeV5Target*                         gpu_prepared_shape_v5_target = nullptr;
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
	static_assert(ScannerPgContext::HighwayPreparationHook{}.empty());
	static_assert(!ScannerPgContext::HighwayPreparationHook{}.valid());

    /*
     * Run the minimap2 pipeline across a batch. DOC0279 §11.3 — per target
     * row, the loop ASSEMBLES the table's active per-sequence minimizer index
     * from its chunked storage (gated by read_index_active + per-row presence)
     * or RECOMPUTES it; the two are mixed freely within one batch.
     *
     * DOC0264 / DOC0274 — production dispatch posture.
     *
     * The Phase C PostgreSQL facade adapter is now wired into the dispatch
     * loop. `scanner_pg_ctx` carries the per-batch identity (db/spc/relfilenode/
     * attno/seq_caches/parent_mctx) plus the per-row optional locator the
     * caller decoded at submit_scan time. When `scanner_pg_ctx` is non-null
     * AND the user has not forced the in-memory baseline via
     * `bioseq.test_force_inmemory_scanner = on`, the loop dispatches:
     *   - target through a source-backed facade when a locator is present,
     *     or an in-memory facade for an inline value;
     *   - query through an in-memory facade over the batch literal.
     * Consumer algorithms see only the alphabet-specialized erased view.
     *
     * NOTE on parameter passing: `target_hdrs` and `query_ids` are full-size
     * `kDispatchBatchCapacity`-element arrays passed by const-ref so the call
     * does not copy them. The result map is RVO'd / moved on return.
     */
    /*
     * Run the minimap2 pipeline across the fixed dispatch-buffer batch shape.
     * The implementation lives in dispatch_processor_impl.cppm so this module
     * interface does not instantiate the full pgseq_supported_alphabets ×
     * storage-width minimap pipeline whenever tuple-buffer/admin code changes.
     */
    [[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const bioseq::pg_types::seq* query_hdr,
        const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
        const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
        const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
        const Minimap2Params& base_params,
        BatchInvocationRole invocation_role,
        std::uint64_t broadcast_query_ordinal,
        ::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        ScannerPgContext* scanner_pg_ctx = nullptr);

};

static_assert(BatchProcessor::exactly_one_native_persisted_reader(
	true, false, false, false));
static_assert(BatchProcessor::exactly_one_native_persisted_reader(
	false, true, false, false));
static_assert(BatchProcessor::exactly_one_native_persisted_reader(
	false, false, true, false));
static_assert(BatchProcessor::exactly_one_native_persisted_reader(
	false, false, false, true));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	false, false, false, false));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	true, true, false, false));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	true, false, true, false));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	true, false, false, true));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	false, true, true, false));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	false, true, false, true));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	false, false, true, true));
static_assert(!BatchProcessor::exactly_one_native_persisted_reader(
	true, true, true, true));
static_assert(BatchProcessor::format6_f5_fallback_allowed(
	true, false, false));
static_assert(BatchProcessor::format6_f5_fallback_allowed(
	false, false, false));
static_assert(!BatchProcessor::format6_f5_fallback_allowed(
	false, true, false));
static_assert(!BatchProcessor::format6_f5_fallback_allowed(
	true, false, true));
static_assert(!BatchProcessor::format6_f5_fallback_allowed(
	false, false, true));

} // namespace bioseq::simd
