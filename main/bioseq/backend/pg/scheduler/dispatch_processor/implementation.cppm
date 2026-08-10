/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor/implementation.cppm
 *
 * Heavy minimap/pipe implementation for batch_processor. Kept out of the
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
 *    - It ignores `active_index` and scans `k_minimizer_index_names` in persistence
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
 * - `bioseq.postgres.scheduler.pipeline_concurrency_mode` and
 *   `bioseq.postgres.scheduler.max_concurrency`: enable task-parallel segment work when
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

#include <bioseq/pg_compat.h>
#include <bioseq/pg_build_profile.h>

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

module bioseq.postgres.scheduler.dispatch_processor;

import :highway_registry;
import :cpu_preparation;
import :typed_flat_selector;
import bioseq.common.logging;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

namespace bioseq::simd {

[[nodiscard]] std::map<int32_t, std::vector<match_result>> batch_processor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const minimap2_parameters& base_params,
    batch_invocation_role invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::ScannerPgContext* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<match_result>> results;
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;

    if (!per_row_query_mode && query_hdr == nullptr) {
        bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor::process_sequences received null query_hdr; returning empty map");
        return results;
    }
	if (!per_row_query_mode
		&& (query_hdr->alphabet_code != alphabet_code
            || query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
        bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor::process_sequences query_hdr typmod ({},{}) disagrees with context ({},{}); refusing to dispatch",
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
        || !highway_binding.kernels->complete()
        || highway_binding.target != highway_plan.planned_target) {
        throw std::logic_error{
            "Highway runtime binding disagrees with the generated target plan"};
    }
	if (scanner_pg_ctx != nullptr
		&& !scanner_pg_ctx->highway_preparation.empty()
		&& !scanner_pg_ctx->highway_preparation.valid()) {
		throw std::logic_error{
			"Highway preparation hook is only partially configured"};
	}
    minimap2_parameters dispatched_params = base_params;
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
		run_pipeline_body();
    } catch (...) {
        bioseq::parallelism::restore_tracked_job_observer(
            previous_observer);
        throw;
    }
    bioseq::parallelism::restore_tracked_job_observer(
        previous_observer);

    return results;
}

batch_processor::CpuPreparedIndexGroup*
batch_processor::CpuPreparedTarget::find_group(
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const std::int32_t max_occ,
		const minimizer_sketch_backend sketch_backend) noexcept {
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

minimap2_parameters batch_processor::effective_params_for_pair(
		const minimap2_parameters& base,
		const std::size_t min_bp) noexcept {
	const bool all_defaults = base.kmer_size == 15
		&& base.window_size == 10
		&& base.min_chain_score == 40
		&& base.min_seed_count == 3;
	if (!all_defaults) {
		return base;
	}
	minimap2_parameters scaled = base;
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

using CpuPreparationStatus = batch_processor::CpuPreparationStatus;
using cpu_preparation_detail::AdmittedCanonicalU32QueryCollector;

[[nodiscard]] bool reserve_parallel_query_sketch_slots(
		std::vector<batch_processor::CpuPreparedQuerySketch>& destination,
		const std::size_t required,
		bioseq::parallelism::memory_permit& permit,
		std::uint64_t& retained_bytes) {
	if (required > destination.max_size()) {
		return false;
	}
	if (required > destination.capacity()) {
		const std::uint64_t old_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(),
				sizeof(batch_processor::CpuPreparedQuerySketch));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				required,
				sizeof(batch_processor::CpuPreparedQuerySketch));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::k_vector_preallocation_headroom_factor);
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
				sizeof(batch_processor::CpuPreparedQuerySketch));
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

} // namespace

void batch_processor::prepare_cpu_shared_query_unions(
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
		const minimap2_parameters& base_params,
		const bool include_persisted_parameter_group,
		const CpuQueryUnionStorage union_storage,
		const CpuPreparationTypmodOps& typmod_ops,
		CpuPreparedTarget& prepared_target) {
	if (!typmod_ops.matches(alphabet_code, storage_unit_byte_width)) {
		throw std::logic_error{
			"CPU preparation typmod operations disagree with the request"};
	}
	const std::uint64_t query_union_prepare_started_ns =
		static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
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
		if (!minimizer_query_length_is_representable<minimap_sketch_data>(
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
		bioseq::parallelism::memory_admission_controller::instance().try_reserve(
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
			bioseq::parallelism::memory_admission_controller::instance().
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
			const minimizer_sketch_backend sketch_backend)
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
						k_vector_preallocation_headroom_factor);
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
	minimizer_sketch_backend parallel_sketch_backend =
		minimizer_sketch_backend::scalar;
	for (std::size_t query_index = 0u;
		 parallel_group_is_uniform && query_index < query_headers.size();
		 ++query_index) {
		const minimap2_parameters effective = effective_params_for_pair(
			base_params,
			std::min<std::uint64_t>(
				target_bp_size, query_bp_sizes[query_index]));
		const minimizer_sketch_backend sketch_backend =
			pipe::target_index_sketch_backend_from_params(effective);
		if (query_index == 0u) {
			parallel_max_occ = effective.max_occ;
			parallel_sketch_backend = sketch_backend;
		} else if (parallel_max_occ != effective.max_occ
			|| parallel_sketch_backend != sketch_backend) {
			parallel_group_is_uniform = false;
		}
	}
	auto& pool = bioseq::parallelism::global_pool::instance();
	parallel_group_is_uniform =
		parallel_group_is_uniform
		&& bioseq::parallelism::has_parallel_dispatch(pool.get_mode())
		&& pool.maximum_concurrency() > 1u;
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
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("BioseqBatchJoin parallel query-sketch slots cannot be admitted")));
			});
			pg_unreachable();
		}
		std::vector<CpuPreparationStatus> statuses(
			query_headers.size(),
			CpuPreparationStatus::pending);
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
								typmod_ops.prepare_independent(
									*query_headers[query_index],
									query_ordinals[query_index],
									group.kmer_size,
									group.window_size,
									group.sketch_backend,
									base_params.highway_kernels,
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
				case CpuPreparationStatus::ready:
					break;
				case CpuPreparationStatus::
						query_length_unsupported:
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("BioseqBatchJoin query exceeds the 28-bit minimizer position domain")));
					});
					pg_unreachable();
				case CpuPreparationStatus::
						admission_unavailable:
				case CpuPreparationStatus::
						collector_growth_failed:
				case CpuPreparationStatus::
						capacity_mismatch:
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("BioseqBatchJoin parallel query sketch exceeded admission")));
					});
					pg_unreachable();
				case CpuPreparationStatus::proof_invalid:
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("BioseqBatchJoin parallel query sketch proof does not match the selected backend")));
					});
					pg_unreachable();
				case CpuPreparationStatus::
						unsupported_typmod:
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("BioseqBatchJoin parallel query preparation received an unsupported typmod")));
					});
					pg_unreachable();
				case CpuPreparationStatus::pending:
					bioseq::postgres::support::invoke_pg_as_cpp([] {
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
					sizeof(minimap_sketch_data));
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("BioseqBatchJoin parallel canonical query union cannot be admitted")));
				});
				pg_unreachable();
			}
			for (const CpuPreparedQuerySketch& retained
					 : group.retained_query_sketches) {
				for (const minimap_sketch_data& minimizer
						 : retained.minimizers) {
					canonical_collector.collect(minimizer);
				}
			}
			if (!canonical_collector.hash_width_valid()) {
				bioseq::postgres::support::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("hash-major canonical query hash exceeds its authenticated u32 profile")));
				});
				pg_unreachable();
			}
			if (!canonical_collector.ok()) {
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
			const minimap2_parameters effective = effective_params_for_pair(
				base_params,
				std::min<std::uint64_t>(
					target_bp_size, query_bp_sizes[query_index]));
			const minimizer_sketch_backend sketch_backend =
				pipe::target_index_sketch_backend_from_params(effective);
			auto append_to = [&](CpuPreparedIndexGroup& group) {
				if (effective_union_storage
						== CpuQueryUnionStorage::canonical_u32
					|| effective_union_storage
						== CpuQueryUnionStorage::
							canonical_u32_sequential_fallback) {
				const auto append_canonical =
					[query_ordinal, &group, &prepared_target, &base_params](
						auto&& append_function) {
					append_function(
						query_ordinal,
							group.kmer_size,
							group.window_size,
							group.sketch_backend,
							base_params.highway_kernels,
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
						[&typmod_ops, query_header](
							auto&&... arguments) {
						typmod_ops.append_header_u32(
							*query_header,
							std::forward<decltype(arguments)>(arguments)...);
					});
				} else {
					append_canonical(
						[&typmod_ops, query_recipe_datums,
						 query_recipe_descriptors, query_index,
						 query_read_session, query_payload_observer](
							auto&&... arguments) {
						typmod_ops.append_recipe_u32(
							query_recipe_datums[query_index],
							query_recipe_descriptors[query_index],
							*query_read_session,
							query_payload_observer,
							std::forward<decltype(arguments)>(arguments)...);
					});
				}
				} else {
					const auto append_union =
						[query_ordinal, &group, &prepared_target, &base_params](
							auto&& append_function) {
						append_function(
						query_ordinal,
							group.kmer_size,
							group.window_size,
							group.sketch_backend,
							base_params.highway_kernels,
							group.query_hash_union,
						*prepared_target.query_union_permit,
						prepared_target.query_union_admission_bytes);
					};
					if (query_header != nullptr) {
						append_union(
							[&typmod_ops,
							 query_header](auto&&... arguments) {
							typmod_ops.append_header_sketch(
								*query_header,
								std::forward<decltype(arguments)>(arguments)...,
								nullptr, nullptr, nullptr);
						});
					} else {
						append_union(
							[&typmod_ops, query_recipe_datums,
							 query_recipe_descriptors, query_index,
							 query_read_session, query_payload_observer](
								auto&&... arguments) {
							typmod_ops.append_recipe_sketch(
								query_recipe_datums[query_index],
								query_recipe_descriptors[query_index],
								*query_read_session,
								query_payload_observer,
								std::forward<decltype(arguments)>(arguments)...,
								nullptr, nullptr, nullptr);
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
					&& pipe::minimizer_range_stats_matches_backend(
						group.sketch_backend,
						retained.proof,
						/*require_nonempty_vector_work=*/false,
						pipe::exact_simd_range_route::require_continuation);
			}
			if (!retained_proofs_match) {
				bioseq::postgres::support::invoke_pg_as_cpp([] {
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
				bioseq::postgres::support::invoke_pg_as_cpp([] {
					ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("hash-major canonical query preparation retained a duplicate sketch owner")));
				});
				pg_unreachable();
			}
			for (const CpuPreparedQuerySketch& retained
				 : group.retained_query_sketches) {
				if (!pipe::minimizer_range_stats_matches_backend(
						group.sketch_backend,
						retained.proof,
						/*require_nonempty_vector_work=*/false,
						pipe::exact_simd_range_route::
							require_continuation)) {
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("hash-major retained query sketch proof is invalid")));
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
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("grouped-SoA query preparation retained a duplicate canonical u32 owner")));
			});
			pg_unreachable();
		}
		if (!group.retained_query_sketches.empty()
			|| group.retained_query_sketches.capacity() != 0u) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("grouped-SoA query preparation retained a hash-major query cache")));
			});
			pg_unreachable();
		}
		/* The target-wide union has tens of millions of trivially-copyable
		 * sketches on the production workload.  The existing in-place radix
		 * implementation avoids comparison-sort's O(Q log Q) key extraction
		 * without introducing another full-size owner or admission peak. */
		if (group.query_hash_union.size() > 1u) {
			std::uint64_t maximum_hash = 0u;
			for (const minimap_sketch_data& minimizer
					: group.query_hash_union) {
				maximum_hash = std::max(maximum_hash, minimizer.kmer());
			}
			if (maximum_hash
					<= (std::numeric_limits<std::uint32_t>::max)()) {
				/* Every currently registered persisted DNA4 profile is at
				 * most 32 bits. Returning the proven narrower key avoids four
				 * known-zero radix passes while the fallback below keeps the
				 * generic 35-bit minimap_sketch_data contract intact. */
				bioseq::sorting::radix_sort(
					group.query_hash_union.data(),
					group.query_hash_union.data()
						+ group.query_hash_union.size(),
					[](const minimap_sketch_data& minimizer) noexcept
							-> std::uint32_t {
						return static_cast<std::uint32_t>(minimizer.kmer());
					});
			} else {
				bioseq::sorting::radix_sort(
					group.query_hash_union.data(),
					group.query_hash_union.data()
						+ group.query_hash_union.size(),
					[](const minimap_sketch_data& minimizer) noexcept
							-> std::uint64_t {
						return minimizer.kmer();
					});
			}
		}
		group.query_hash_union.erase(
			std::unique(
				group.query_hash_union.begin(),
				group.query_hash_union.end(),
				[](const minimap_sketch_data& lhs,
				   const minimap_sketch_data& rhs) noexcept {
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
						sizeof(minimap_sketch_data))));
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
		bioseq::postgres::support::invoke_pg_as_cpp([retained, admitted] {
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
							sizeof(minimap_sketch_data))));
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
		bioseq::postgres::support::invoke_pg_as_cpp([] {
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
		static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
	prepared_target.query_union_prepare_host_wall_ns =
		query_union_prepare_finished_ns >= query_union_prepare_started_ns
			? std::max<std::uint64_t>(
				1u,
				query_union_prepare_finished_ns
					- query_union_prepare_started_ns)
			: 1u;
	prepared_target.unions_ready = true;
}

bool batch_processor::publish_compact_direct_query_set_proof(
		CpuPreparedTarget& prepared_target,
		CpuPreparedIndexGroup& group,
		const bioseq::indexing::compact_direct_query_set_stats& proof) noexcept {
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
			!= bioseq::indexing::k_compact_direct_query_set_fnv_offset
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
			bioseq::indexing::k_compact_direct_query_set_fnv_prime;
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

void batch_processor::finalize_pending_compact_direct_query_set_proof_fallback(
		CpuPreparedTarget& prepared_target) {
	if (prepared_target.query_set_proof_ready()) return;
	if (!prepared_target.unions_ready
		|| prepared_target.groups.size() != 1u) {
		throw bioseq::indexing::persisted_minimizer_lookup_error(
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
		throw bioseq::indexing::persisted_minimizer_lookup_error(
			"compact-direct query-set fallback retained state is incomplete");
	}

	std::uint64_t occurrence_count = 0u;
	std::optional<std::uint64_t> previous_query_ordinal;
	for (const CpuPreparedQuerySketch& retained
			: group.retained_query_sketches) {
		if ((previous_query_ordinal
				&& *previous_query_ordinal >= retained.query_ordinal)
			|| !pipe::minimizer_range_stats_matches_backend(
				group.sketch_backend,
				retained.proof,
				/*require_nonempty_vector_work=*/false,
				pipe::exact_simd_range_route::require_continuation)) {
			throw bioseq::indexing::persisted_minimizer_lookup_error(
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
			bioseq::parallelism::k_vector_preallocation_headroom_factor);
	if (occurrence_count
			== (std::numeric_limits<std::uint64_t>::max)()
		|| occurrence_count
			> (std::numeric_limits<std::size_t>::max)()
		|| requested_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| allocation_headroom
			== (std::numeric_limits<std::uint64_t>::max)()) {
		throw bioseq::indexing::persisted_minimizer_lookup_error(
			"compact-direct query-set fallback size overflow");
	}
	auto fallback_permit =
		bioseq::parallelism::memory_admission_controller::instance().try_reserve(
			allocation_headroom);
	if (!fallback_permit) {
		throw bioseq::indexing::persisted_minimizer_lookup_error(
			bioseq::indexing::persisted_minimizer_lookup_error::Reason::
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
			throw bioseq::indexing::persisted_minimizer_lookup_error(
				bioseq::indexing::persisted_minimizer_lookup_error::Reason::
					admission_or_allocation,
				"compact-direct query-set fallback exceeded admission");
		}
		fallback_permit->shrink_to(measured_bytes);
		for (const CpuPreparedQuerySketch& retained
				: group.retained_query_sketches) {
			for (const minimap_sketch_data& minimizer : retained.minimizers) {
				if (minimizer.kmer()
						> (std::numeric_limits<std::uint32_t>::max)()) {
					throw bioseq::indexing::persisted_minimizer_lookup_error(
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
		const bioseq::indexing::compact_direct_query_set_stats proof{
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
			throw bioseq::indexing::persisted_minimizer_lookup_error(
				"compact-direct query-set fallback publication was rejected");
		}
	} catch (const std::bad_alloc&) {
		throw bioseq::indexing::persisted_minimizer_lookup_error(
			bioseq::indexing::persisted_minimizer_lookup_error::Reason::
				admission_or_allocation,
			"compact-direct query-set fallback allocation failed");
	}
}

void batch_processor::prepare_cpu_persisted_native_target(
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
		const minimap2_parameters& base_params,
		const bioseq::storage::indexing::borrowed_executor_read_context&
			executor_read,
		const CpuPreparationTypmodOps& typmod_ops,
		CpuPreparedTarget& prepared_target) {
	if (!typmod_ops.matches(alphabet_code, storage_unit_byte_width)) {
		throw std::logic_error{
			"persisted CPU preparation typmod operations disagree with the request"};
	}
	using namespace bioseq::storage::indexing;
	using bioseq::indexing::minimizer_generation_encoding_error;
	using bioseq::indexing::minimizer_generation_layout;
	const auto fail = [](std::string message) -> void {
		bioseq::postgres::support::invoke_pg_as_cpp([&] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("required compatible persisted minimizer index is invalid"),
				 errdetail("%s", message.c_str()),
				 errhint("Rebuild the exact revision-3 grouped-SoA READY generation; "
					"the required route has no recompute or cache fallback.")));
		});
	};
	const auto unavailable = [](std::string message) -> void {
		bioseq::postgres::support::invoke_pg_as_cpp([&] {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("required compatible persisted minimizer index unavailable"),
				 errdetail("%s", message.c_str()),
				 errhint("Build the exact revision-3 grouped-SoA READY generation; "
					"the required route has no recompute or cache fallback.")));
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
	const minimizer_sketch_backend sketch_backend =
		pipe::target_index_sketch_backend_from_params(base_params);

	minimizer_generation_administration_identity identity{
		.database_oid = database_oid,
		.tablespace_oid = tablespace_oid,
		.main_relfilenode = main_relfilenode,
		.user_relid = user_relid,
		.column_attno = column_attno,
		.index_name = std::string{index_name},
	};
	auto source_proof = derive_minimizer_generation_catalog_source_proof_pg(
		identity, sequence_rowno, executor_read);
	const minimizer_generation_source_locator source =
		source_proof.source_for_diagnostics();
	const minimizer_generation_catalog_source_proof_counters source_counters =
		source_proof.counters_for_diagnostics();
	const std::uint64_t source_content_checksum =
		source_proof.source_content_checksum_for_diagnostics();
	if (source.sequence_rowno != sequence_rowno
		|| source.locator.logical_length != target_bp_size
		|| source.alphabet_code != alphabet_code
		|| source.storage_unit_byte_width != storage_unit_byte_width) {
		fail("authoritative source identity disagrees with the prepared target");
		return;
	}
	identity.table_name = format_seqidx_name(
		index_name, database_oid, tablespace_oid, main_relfilenode,
		column_attno, source.locator.bucket_index, source.locator.shard_index);

	auto opened_hash_major = open_exact_hash_major_minimizer_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read);
	if (!opened_hash_major.session && !opened_hash_major.head_absent) {
		switch (opened_hash_major.relation_failure.kind) {
			case minimizer_generation_relation_kind::missing:
				unavailable(fmt::format(
					"revision-3 generation relation {} is absent",
					identity.table_name));
				return;
			case minimizer_generation_relation_kind::legacy_exact:
				unavailable(fmt::format(
					"relation {} is the exact legacy {} schema and has no "
					"revision-3 READY generation",
					identity.table_name, index_name));
				return;
			case minimizer_generation_relation_kind::malformed:
				fail(fmt::format(
					"generation relation {} is neither the canonical revision-3 "
					"schema nor the exact legacy {} schema; revision3_schema_error={}",
					identity.table_name, index_name,
					minimizer_generation_schema_error_name(
						opened_hash_major.relation_failure.revision3_error)));
				return;
			case minimizer_generation_relation_kind::revision3_exact:
				fail("hash-major relation binder returned an invalid success classification");
				return;
			}
	}
	if (opened_hash_major.session && opened_hash_major.head_absent) {
		fail("hash-major binder returned both a session and an absence proof");
		return;
	}
	bool hash_major_profile_applicable = false;
	if (opened_hash_major.session) {
		index_storage_hash_major_minimizer_read_session& hash_major_session =
			*opened_hash_major.session;
		const auto scheme =
			bioseq::indexing::minimizer_scheme_identity_for_backend(
				sketch_backend);
		const auto producer =
			bioseq::indexing::
				minimizer_producer_backend_identity_for_backend(
					sketch_backend);
		const auto hash_major_candidate_manifest =
			hash_major_session.manifest();
		const auto compatibility =
			hash_major_candidate_manifest.compatibility;
		if (!bioseq::indexing::is_valid_hash_major_minimizer_compatibility(
				compatibility, hash_major_candidate_manifest.target_length)) {
			fail("hash-major READY head names an invalid compatibility profile");
			return;
		}
		hash_major_profile_applicable =
			compatibility.hash_scheme_id == scheme.id
			&& compatibility.hash_scheme_version == scheme.version
			&& compatibility.alphabet_code == alphabet_code
			&& compatibility.kmer_size == 15u
			&& compatibility.window_size == 10u
			&& compatibility.hash_width_bytes == sizeof(std::uint32_t)
			&& compatibility.producer_backend_id
				== static_cast<std::uint16_t>(producer.id)
			&& compatibility.producer_backend_version == producer.version;
		if (!hash_major_profile_applicable) {
			/* A self-authenticated but route-inapplicable hash-major profile is
			 * the only present-head fallback case.  No source proof has been
			 * consumed and no directory/control/posting callback has run, so
			 * the independent READY grouped-SoA sentinel may still be opened. */
			hash_major_session.close_metadata_read();
			opened_hash_major.session.reset();
		} else {
			const bioseq::indexing::expected_hash_major_minimizer_generation
				hash_major_expected =
					hash_major_session.validate_source_attestation(
						std::move(source_proof));
			if (hash_major_expected.generation_id == 0u
			|| hash_major_expected.query_key != sequence_rowno
			|| hash_major_expected.source_epoch != source.locator.source_epoch
			|| hash_major_expected.source_content_crc64
				!= source_content_checksum
			|| hash_major_expected.target_length != target_bp_size
			|| hash_major_expected.compatibility != compatibility) {
				fail("hash-major authenticated source binding disagrees with the active CPU route");
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
				typmod_ops,
				prepared_target);
			if (!prepared_target.unions_ready
				|| prepared_target.groups.size() != 1u) {
				fail("hash-major canonical query preparation did not yield one group");
				return;
			}
			CpuPreparedIndexGroup& hash_major_group =
				prepared_target.groups.front();
			if (hash_major_group.kmer_size != 15
				|| hash_major_group.window_size != 10
				|| hash_major_group.max_occ < 0
				|| hash_major_group.sketch_backend != sketch_backend
				|| !hash_major_group.query_hash_union.empty()
				|| hash_major_group.query_hash_union.capacity() != 0u
				|| hash_major_group.persisted_fragment_mapping_guard
				|| hash_major_group.persisted_fragment_reader
				|| hash_major_group.persisted_grouped_soa_reader
				|| hash_major_group.persisted_hash_major_reader
				|| hash_major_group.target_index) {
				fail("hash-major canonical query group violates exclusive ownership");
				return;
			}
			const bioseq::indexing::canonical_query_hash_view canonical_view{
				.values32 = std::span<const std::uint32_t>{
					hash_major_group.canonical_query_hash_union_u32},
				.width_bytes = sizeof(std::uint32_t),
				.storage_width_bytes = sizeof(std::uint32_t),
				.compatibility_digest =
					bioseq::indexing::
						hash_major_minimizer_compatibility_digest(
							compatibility),
			};
			const std::uint64_t hash_major_load_started_ns =
				static_cast<std::uint64_t>(
					bioseq::instrumentation::clock::now_ns());
			auto hash_major_reader =
				bioseq::indexing::hash_major_minimizer_selective_reader::open(
					bioseq::parallelism::memory_admission_controller::instance(),
					hash_major_session.encoded_head(),
					hash_major_session.encoded_manifest(),
					hash_major_expected,
					hash_major_session.source_ref(),
					canonical_view,
					static_cast<std::uint64_t>(hash_major_group.max_occ));
			if (!hash_major_reader) {
				fail(fmt::format(
					"hash-major selective reader rejected authenticated storage error={}",
					static_cast<unsigned>(hash_major_reader.error())));
				return;
			}
			const auto hash_major_head = hash_major_session.head();
			const auto hash_major_manifest = hash_major_session.manifest();
			minimizer_generation_read_stats read_stats =
				hash_major_session.finish();
			const std::uint64_t hash_major_load_finished_ns =
				static_cast<std::uint64_t>(
					bioseq::instrumentation::clock::now_ns());
			const std::uint64_t hash_major_load_ns =
				hash_major_load_finished_ns >= hash_major_load_started_ns
					? std::max<std::uint64_t>(
						1u,
						hash_major_load_finished_ns
							- hash_major_load_started_ns)
					: 1u;
			bioseq_log<logging_level::debug>(
				"[bioseq] hash-major selective reader: load_ns={} "
				"rows={} payload_bytes={} retained_positions={} "
				"stored_position_width={}",
				static_cast<unsigned long long>(hash_major_load_ns),
				static_cast<unsigned long long>(
					read_stats.hash_major.directory_rows
					+ read_stats.hash_major.control_rows
					+ read_stats.hash_major.posting_rows),
				static_cast<unsigned long long>(
					read_stats.hash_major.directory_bytes
					+ read_stats.hash_major.control_bytes
					+ read_stats.hash_major.posting_bytes),
				static_cast<unsigned long long>(
					hash_major_reader->memory_stats()
						.retained_position_count),
				static_cast<unsigned>(
					hash_major_manifest.compatibility
						.stored_posting_width_bytes));
			const auto& memory_stats =
				hash_major_reader->memory_stats();
		const std::uint64_t peak_admitted_bytes = std::max({
			memory_stats.directory_phase_peak_admitted_bytes,
			memory_stats.control_phase_peak_admitted_bytes,
			memory_stats.posting_phase_peak_admitted_bytes,
		});
		const std::uint64_t admitted_bytes =
			hash_major_reader->admitted_bytes();
		const std::uint64_t authorized_posting_bytes =
			bioseq::parallelism::saturating_mul(
				memory_stats.retained_position_count,
				hash_major_manifest.compatibility
					.stored_posting_width_bytes);
		if (hash_major_reader->target_length() != target_bp_size
			|| hash_major_head.generation_id != hash_major_expected.generation_id
			|| hash_major_head.source_identity
				!= hash_major_manifest.source_identity
			|| hash_major_manifest.compatibility != compatibility
			|| read_stats.relation_opens != 1u
			|| read_stats.head_scans != 1u
			|| read_stats.source_parent_rows_read != 1u
			|| read_stats.source_catalog_rows_read != 1u
			|| read_stats.source_catalog_index_scans != 1u
			|| read_stats.source_fragment_rows_read != 0u
			|| read_stats.source_decoded_bp != 0u
			|| read_stats.hash_major.format_revision
				!= bioseq::indexing::k_hash_major_minimizer_format_revision
			|| read_stats.hash_major.sessions != 1u
			|| read_stats.hash_major.generation_id
				!= hash_major_head.generation_id
			|| read_stats.hash_major.source_identity
				!= hash_major_head.source_identity
			|| read_stats.hash_major.source_content_crc64
				!= hash_major_head.source_content_crc64
			|| read_stats.hash_major.compatibility_digest
				!= hash_major_head.compatibility_digest
			|| memory_stats.directory_stream_crc_receipts != 1u
			|| memory_stats.control_block_crc_receipts
				!= memory_stats.selected_block_count
			|| memory_stats.control_semantic_receipts
				!= memory_stats.selected_block_count
			|| memory_stats.authorized_posting_bytes
				!= authorized_posting_bytes
			|| memory_stats.generic_source_fallback_calls != 0u
			|| memory_stats.peak_admitted_bytes
				!= peak_admitted_bytes
			|| memory_stats.retained_owner_bytes != admitted_bytes
			|| read_stats.hash_major.directory_row_crc_receipts
				!= read_stats.hash_major.directory_rows
			|| read_stats.hash_major.control_row_crc_receipts
				!= read_stats.hash_major.control_rows
			|| read_stats.hash_major.posting_row_crc_receipts
				!= read_stats.hash_major.posting_rows
			|| read_stats.hash_major.posting_ranges_delivered
				!= memory_stats.posting_ranges_delivered
			|| read_stats.hash_major.authorized_posting_bytes
				!= memory_stats.authorized_posting_bytes
			|| memory_stats.posting_semantic_receipts
				!= memory_stats.posting_ranges_delivered
			|| memory_stats.posting_range_crc_receipts
				> memory_stats.posting_ranges_delivered
			|| !source_counters.timed_equation_holds()) {
			fail("hash-major READY identity/profile/read-work proof disagrees");
			return;
		}
		auto& hash_major_stats = read_stats.hash_major;
		hash_major_stats.format_revision =
			bioseq::indexing::k_hash_major_minimizer_format_revision;
		hash_major_stats.sessions = 1u;
		hash_major_stats.generation_id = hash_major_head.generation_id;
		hash_major_stats.source_identity =
			hash_major_head.source_identity;
		hash_major_stats.source_content_crc64 =
			hash_major_head.source_content_crc64;
		hash_major_stats.compatibility_digest =
			hash_major_head.compatibility_digest;
		hash_major_stats.directory_stream_crc_receipts =
			memory_stats.directory_stream_crc_receipts;
		hash_major_stats.control_block_crc_receipts =
			memory_stats.control_block_crc_receipts;
		hash_major_stats.control_semantic_receipts =
			memory_stats.control_semantic_receipts;
		hash_major_stats.posting_range_crc_receipts =
			memory_stats.posting_range_crc_receipts;
		hash_major_stats.posting_semantic_receipts =
			memory_stats.posting_semantic_receipts;
		hash_major_stats.query_count =
			memory_stats.query_count;
		hash_major_stats.selected_block_count =
			memory_stats.selected_block_count;
		hash_major_stats.accepted_hash_count =
			memory_stats.accepted_hash_count;
		hash_major_stats.retained_position_count =
			memory_stats.retained_position_count;
		hash_major_stats.directory_stage_peak_bytes =
			memory_stats.directory_phase_peak_admitted_bytes;
		hash_major_stats.control_stage_peak_bytes =
			memory_stats.control_phase_peak_admitted_bytes;
		hash_major_stats.posting_stage_peak_bytes =
			memory_stats.posting_phase_peak_admitted_bytes;
		hash_major_stats.peak_admitted_bytes =
			memory_stats.peak_admitted_bytes;
		hash_major_stats.retained_owner_bytes =
			memory_stats.retained_owner_bytes;
		hash_major_stats.admitted_bytes = admitted_bytes;
		hash_major_stats.generic_source_fallback_calls =
			memory_stats.generic_source_fallback_calls;
		hash_major_stats.failures = 0u;
		hash_major_stats.saturated = false;
		hash_major_group.persisted_hash_major_reader.emplace(
			std::move(*hash_major_reader));
		hash_major_group.target_index_source =
			::bioseq::pipeline_stats::target_index_source::persisted;
		hash_major_group.persisted_generation_id =
			hash_major_head.generation_id;
		hash_major_group.persisted_hash_major_source_identity =
			hash_major_head.source_identity;
		hash_major_group.persisted_hash_major_source_identity_present = true;
		/* The legacy u64 pipeline correlation value is deliberately not used
		 * to authenticate hash-major; correctness above is bound to all 128
		 * source-identity bits. */
		hash_major_group.persisted_source_identity =
			hash_major_source_diagnostic_token(
				hash_major_head.source_identity);
		hash_major_group.persisted_read_stats = read_stats;
		hash_major_group.persisted_reader_loads = 1u;
		hash_major_group.build_set_checksum_present = true;
		hash_major_group.build_set_checksum =
			hash_major_group.key_set_checksum;
		++prepared_target.physical_target_index_reads;
		return;
		}
	}
	/* The binder's head_absent result is an authenticated absence of the
	 * distinct hash-major sentinel.  Together with the inapplicable-profile case
	 * above, this is before every hash-major stream payload callback and therefore the
	 * only clean boundary at which grouped-SoA may be tried. */
	if (!hash_major_grouped_soa_fallback_allowed(
			opened_hash_major.head_absent,
			hash_major_profile_applicable,
			/*stream_io_started=*/false)) {
		fail("hash-major dispatch crossed the fail-closed fallback boundary");
		return;
	}
	opened_hash_major.session.reset();
	const auto expected =
		bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
			minimizer_generation_layout::grouped_soa_segment_local,
			sketch_backend,
			source.alphabet_code,
			source.storage_unit_byte_width,
			15u,
			10u,
			bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
	if (!expected) {
		fail(fmt::format("grouped-SoA compatibility rejected wire_error={}",
			static_cast<unsigned>(expected.error())));
		return;
	}
	const auto configured_mpf_grouped_soa_lookup_route =
		bioseq::aligner::settings::fragment_minimizer::lookup_route.get_value();
	const bool compact_h32_direct_configured =
		configured_mpf_grouped_soa_lookup_route
		== bioseq::aligner::fragment_minimizer_lookup_route::
			direct_compact_hash_lookup;
	const bool compact_h32_direct_requested =
		compact_h32_direct_configured
		&& index_name
			== bioseq::indexing::k_minimizers_per_fragment_index_name;
	auto opened = open_exact_minimizer_generation_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read,
		*expected);
	if (!opened) {
		switch (opened.failure.kind) {
			case minimizer_generation_relation_kind::missing:
				unavailable(fmt::format(
					"revision-3 generation relation {} is absent",
					identity.table_name));
				return;
			case minimizer_generation_relation_kind::legacy_exact:
				unavailable(fmt::format(
					"relation {} is the exact legacy {} schema and has no "
					"revision-3 READY generation",
					identity.table_name, index_name));
				return;
			case minimizer_generation_relation_kind::malformed:
				fail(fmt::format(
					"generation relation {} is neither the canonical revision-3 "
					"schema nor the exact legacy {} schema; revision3_schema_error={}",
					identity.table_name, index_name,
					minimizer_generation_schema_error_name(
						opened.failure.revision3_error)));
				return;
			case minimizer_generation_relation_kind::revision3_exact:
				fail("generation relation binder returned an invalid success classification");
				return;
		}
	}
	index_storage_fragment_generation_read_session& session = *opened.session;
	const auto head = session.read_active_head_once();
	std::optional<std::span<const std::byte>>
		authenticated_grouped_soa_directory{};
	const minimizer_generation_read_metadata* metadata = nullptr;
	minimizer_generation_manifest manifest{};
	if (compact_h32_direct_requested) {
		manifest = session.read_manifest_once_without_directory(head);
	} else {
		metadata = &session.read_manifest_directory_once(head);
		manifest = metadata->manifest;
		authenticated_grouped_soa_directory =
			metadata->directory.encoded_bytes();
	}
	if (manifest.layout
			!= minimizer_generation_layout::grouped_soa_segment_local) {
		fail("READY generation is not the current grouped-SoA layout");
		return;
	}
	const bool mapped_mpf_grouped_soa = index_name
			== bioseq::indexing::k_minimizers_per_fragment_index_name;
	const bool full_mps_grouped_soa = index_name
			== bioseq::indexing::k_minimizers_per_sequence_index_name;
	if (!mapped_mpf_grouped_soa && !full_mps_grouped_soa) {
		fail("grouped-SoA READY generation is bound to an unsupported logical index");
		return;
	}
	if (compact_h32_direct_requested && !mapped_mpf_grouped_soa) {
		fail("compact-H32 direct lookup requires an MPF grouped-SoA READY generation");
		return;
	}
	const auto& selected_expected = *expected;
	session.validate_source_attestation(std::move(source_proof));
	auto& query_prep_pool = bioseq::parallelism::global_pool::instance();
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
			>= bioseq::parallelism::task_parallel_minimums::k_query_count
		&& bioseq::parallelism::has_parallel_tasks(query_prep_pool.get_mode())
		&& query_prep_pool.maximum_concurrency() > 1u;
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
		mapped_mpf_grouped_soa
			? (defer_compact_direct_query_set_proof
				? CpuQueryUnionStorage::retained_sketch_only
				: CpuQueryUnionStorage::canonical_u32)
			: CpuQueryUnionStorage::minimizer_sketch,
		typmod_ops,
		prepared_target);
	if (!prepared_target.unions_ready
		|| prepared_target.groups.size() != 1u) {
		fail("grouped-SoA query preparation did not yield one group");
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
		|| (mapped_mpf_grouped_soa
			? (!group.query_hash_union.empty()
				|| group.query_hash_union.capacity() != 0u)
			: (!group.canonical_query_hash_union_u32.empty()
				|| group.canonical_query_hash_union_u32.capacity() != 0u))
		|| group.persisted_fragment_mapping_guard
		|| group.persisted_fragment_reader
		|| group.persisted_grouped_soa_reader
		|| group.persisted_hash_major_reader
		|| group.target_index) {
		fail("grouped-SoA parameter group is not the pinned exclusive cold-read shape");
		return;
	}
	// MPF Format 5 remains a collection of independent physical-fragment
	// indexes. The mmap image is the byte-exact artifact produced by the
	// writer's complete authenticated READY reread, fsync, and durable rename.
	// Cold open checks the exact generation identity and prepares only fragment
	// address plans. Query-wave max-occ reduction is fused into the parallel
	// fragment probe; no target-wide posting/count owner is constructed.
	if (mapped_mpf_grouped_soa) {
		using fragment_reader_type =
			bioseq::indexing::prepared_minimizer_fragment_reader;
		const std::uint64_t fragment_load_started_ns =
			static_cast<std::uint64_t>(
				bioseq::instrumentation::clock::now_ns());
		const auto storage_descriptor =
			session.read_live_storage_descriptor_once(identity);
		auto mapped_image = open_exact_minimizer_fragment_image_pg(
			identity, sequence_rowno, head, manifest,
			storage_descriptor,
			compact_h32_direct_requested
				? minimizer_fragment_image_mapped_open_validation::bounded_header_only
				: minimizer_fragment_image_mapped_open_validation::full_metadata);
		std::optional<fragment_reader_type> fragment_reader{};
		minimizer_generation_read_stats read_stats{};
		if (compact_h32_direct_requested) {
			const auto direct_profile =
				bioseq::indexing::derive_grouped_soa_minimizer_profile(
					manifest);
			if (!direct_profile
				|| direct_profile->max_hash == 0u
				|| direct_profile->max_hash
					> (std::numeric_limits<std::uint32_t>::max)()
				|| direct_profile->max_occ_cap == 0u
				|| direct_profile->max_occ_cap
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
			const bioseq::indexing::compact_hash32_bounded_open_options
				direct_open_options{
					.expected_identity = bioseq::indexing::direct_index_identity{
						.generation_id = head.generation_id,
						.source_identity = head.source_identity,
						.compatibility_checksum =
							head.compatibility_checksum,
						.target_length = manifest.target_length,
					},
					.expected_max_occ_cap = static_cast<std::uint32_t>(
						direct_profile->max_occ_cap),
					.expected_geometry =
						bioseq::indexing::compact_hash32_geometry{
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
				owned_compact_hash32_direct_index_view::bind(
					bounded_direct->image_owner(), bounded_direct->view());
			if (!owned_direct) {
				fail(fmt::format(
					"compact-H32 owner binding rejected wire_error={}",
					static_cast<unsigned>(owned_direct.error())));
				return;
			}
			auto made = fragment_reader_type::open_compact_direct(
				bioseq::parallelism::memory_admission_controller::instance(),
				head, manifest, selected_expected,
				static_cast<std::uint64_t>(group.max_occ),
				std::move(*owned_direct), selected_page_adviser);
			if (!made) {
				fail(fmt::format(
					"compact-H32 fragment reader rejected wire_error={}",
					static_cast<unsigned>(made.error())));
				return;
			}
			fragment_reader.emplace(std::move(*made));
			read_stats = session.finish_mapped_compact_direct();
		} else {
			if (!authenticated_grouped_soa_directory) {
				fail("grouped-SoA control-scan route has no authenticated directory");
				return;
			}
			const std::span<const std::byte> mapped_payload =
				mapped_image.payload();
			auto mapped_payload_owner =
				mapped_image.borrow_mapping_token();
			auto made = fragment_reader_type::open(
				bioseq::parallelism::memory_admission_controller::instance(),
				head, manifest, selected_expected,
				*authenticated_grouped_soa_directory,
				static_cast<std::uint64_t>(group.max_occ),
				std::move(mapped_payload_owner),
				mapped_payload);
			if (!made) {
				fail(fmt::format(
					"fragment-native grouped-SoA reader rejected wire_error={}",
					static_cast<unsigned>(made.error())));
				return;
			}
			fragment_reader.emplace(std::move(*made));
			read_stats = session.finish_mapped_minimizer_fragment_payload();
		}
		const std::uint64_t fragment_load_finished_ns =
			static_cast<std::uint64_t>(
				bioseq::instrumentation::clock::now_ns());
		group.persisted_reader_load_host_wall_ns =
			fragment_load_finished_ns >= fragment_load_started_ns
				? std::max<std::uint64_t>(
					1u,
					fragment_load_finished_ns
						- fragment_load_started_ns)
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
			minimizer_generation_head::encoded_bytes
			+ bioseq::indexing::minimizer_manifest_encoded_bytes(
				manifest.layout)
			+ read_stats.directory_bytes_read;
		const bool route_read_proof_matches =
			compact_h32_direct_requested
			? fragment_reader->compact_direct_ready()
				&& fragment_reader->compact_direct_only()
				&& fragment_reader->mapped_grouped_soa_payload_bytes() == 0u
				&& fragment_reader->grouped_soa_directory_entry_count() == 0u
				&& read_stats.directory_chunks_read == 0u
				&& read_stats.directory_bytes_read == 0u
			: !fragment_reader->compact_direct_only()
				&& fragment_reader->mapped_grouped_soa_payload_bytes()
					== manifest.payload_bytes
				&& read_stats.directory_chunks_read
					== manifest.directory_chunk_count
				&& read_stats.directory_bytes_read
					== manifest.directory_bytes;
		const bool read_proof_matches =
			fragment_reader->ready()
			&& route_read_proof_matches
			&& fragment_reader->target_length()
				== prepared_target.target_bp_size
			&& manifest.kmer_size == group.kmer_size
			&& manifest.window_size == group.window_size
			&& manifest.minimizer_scheme_id == scheme.id
			&& manifest.minimizer_scheme_revision == scheme.version
			&& manifest.producer_backend_id == producer_backend.id
			&& manifest.producer_identity_revision
				== producer_backend.version
			&& manifest.max_occ_cap
				== bioseq::indexing::k_grouped_soa_default_max_occurrence_cap
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
			fail("grouped-SoA READY identity/backend/scheme/read-work proof disagrees");
			return;
		}

		auto mapping_guard =
			std::move(mapped_image).release_mapping_lease_guard();
		if (!mapping_guard) {
			fail("grouped-SoA mapped reader lost its mmap/fd lifetime authority");
			return;
		}
		group.persisted_fragment_mapping_guard.emplace(
			std::move(mapping_guard));
		group.persisted_fragment_reader.emplace(
			std::move(*fragment_reader));
		group.persisted_fragment_lookup_route =
			compact_h32_direct_requested
			? bioseq::indexing::minimizer_fragment_lookup_route::compact_h32_direct
			: bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans;
		group.target_index_source =
			::bioseq::pipeline_stats::target_index_source::persisted;
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

	/* MPS grouped-SoA remains a sequence-wide persisted index.  Unlike the MPF
	 * route above, it has no authority to open the MPF-only mmap sidecar: read
	 * and authenticate the complete PostgreSQL payload into the established
	 * query-selected Shape-v5 reader. */
	if (full_mps_grouped_soa) {
		using grouped_soa_reader_type =
			bioseq::indexing::prepared_grouped_soa_query_reader;
		if (!authenticated_grouped_soa_directory) {
			fail("MPS grouped-SoA route has no authenticated directory");
			return;
		}
		const std::uint64_t grouped_soa_load_started_ns =
			static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
		auto made = grouped_soa_reader_type::begin(
			bioseq::parallelism::memory_admission_controller::instance(),
			head, manifest, selected_expected,
			*authenticated_grouped_soa_directory, group.query_hash_union,
			static_cast<std::uint64_t>(group.max_occ));
		if (!made) {
			fail(fmt::format(
				"MPS grouped-SoA reader begin rejected wire_error={}",
				static_cast<unsigned>(made.error())));
			return;
		}
		std::optional<grouped_soa_reader_type> grouped_soa_reader{
			std::in_place, std::move(*made)};
		/* The reader owns the exact compact query-hash image from this point.
		 * Release the scheduler copy before allocating retained payload state. */
		if (!prepared_target.release_group_query_hash_union_noexcept(group)) {
			fail("MPS grouped-SoA canonical query-union release disagrees with admission ownership");
			return;
		}

		std::optional<minimizer_generation_encoding_error> callback_error;
		std::exception_ptr callback_exception;
		const auto accept = [
				&callback_error,
				&callback_exception,
				&grouped_soa_reader](const bioseq::indexing::
				validated_minimizer_generation_row_receipt& receipt) noexcept {
			if (callback_error || callback_exception) return;
			try {
				auto accepted = grouped_soa_reader->accept(receipt);
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
				"MPS grouped-SoA payload ingest rejected wire_error={}",
				static_cast<unsigned>(*callback_error)));
			return;
		}
		if (auto finished = grouped_soa_reader->finish(); !finished) {
			fail(fmt::format(
				"MPS grouped-SoA reader final validation rejected wire_error={}",
				static_cast<unsigned>(finished.error())));
			return;
		}

		const minimizer_generation_read_stats read_stats = session.finish();
		const std::uint64_t grouped_soa_load_finished_ns =
			static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
		group.persisted_reader_load_host_wall_ns =
			grouped_soa_load_finished_ns >= grouped_soa_load_started_ns
				? std::max<std::uint64_t>(
					1u,
					grouped_soa_load_finished_ns - grouped_soa_load_started_ns)
				: 1u;
		const auto scheme =
			bioseq::indexing::minimizer_scheme_identity_for_backend(
				group.sketch_backend);
		const auto producer_backend =
			bioseq::indexing::minimizer_producer_backend_identity_for_backend(
				group.sketch_backend);
		const bool read_proof_matches =
			grouped_soa_reader->ready()
			&& grouped_soa_reader->target_length()
				== prepared_target.target_bp_size
			&& manifest.kmer_size == group.kmer_size
			&& manifest.window_size == group.window_size
			&& manifest.minimizer_scheme_id == scheme.id
			&& manifest.minimizer_scheme_revision == scheme.version
			&& manifest.producer_backend_id == producer_backend.id
			&& manifest.producer_identity_revision
				== producer_backend.version
			&& manifest.max_occ_cap
				== bioseq::indexing::k_grouped_soa_default_max_occurrence_cap
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
			fail("MPS grouped-SoA READY identity/backend/scheme/read-work proof disagrees");
			return;
		}

		group.persisted_grouped_soa_reader.emplace(
			std::move(*grouped_soa_reader));
		group.target_index_source =
			::bioseq::pipeline_stats::target_index_source::persisted;
		group.persisted_generation_id = head.generation_id;
		group.persisted_source_identity = head.source_identity;
		group.persisted_read_stats = read_stats;
		group.persisted_reader_loads = 1;
		group.build_set_checksum_present = true;
		group.build_set_checksum = group.key_set_checksum;
		++prepared_target.physical_target_index_reads;
		return;
	}

}

void batch_processor::prepare_gpu_persisted_grouped_soa_target(
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
		const minimap2_parameters& params,
		const bioseq::storage::indexing::borrowed_executor_read_context&
			executor_read,
		gpu_prepared_grouped_soa_target& prepared_target) {
	using namespace bioseq::storage::indexing;
	using bioseq::indexing::gpu_grouped_soa_ingest_error;
	using bioseq::indexing::minimizer_generation_layout;
	const auto fail = [](const int sqlstate, std::string message) -> void {
		bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
			ereport(ERROR,
				(errcode(sqlstate),
				 errmsg("required persisted GPU grouped-SoA target is unavailable"),
				 errdetail("%s", message.c_str()),
				 errhint("Publish a matching grouped-SoA READY generation; "
					"GPU preparation has no CPU, SIMD, recompute, or legacy-index fallback.")));
		});
	};
	prepared_target = gpu_prepared_grouped_soa_target{};
	const minimizer_sketch_backend backend =
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
		|| !pipe::is_persisted_grouped_soa_gpu_preparation_tuple_valid(
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

	minimizer_generation_administration_identity identity{
		.database_oid = database_oid,
		.tablespace_oid = tablespace_oid,
		.main_relfilenode = main_relfilenode,
		.user_relid = user_relid,
		.column_attno = column_attno,
		.index_name = std::string{index_name},
	};
	auto source_proof = derive_minimizer_generation_catalog_source_proof_pg(
		identity, sequence_rowno, executor_read);
	const minimizer_generation_source_locator source =
		source_proof.source_for_diagnostics();
	const minimizer_generation_catalog_source_proof_counters source_counters =
		source_proof.counters_for_diagnostics();
	if (source.alphabet_code != alphabet_code
		|| source.storage_unit_byte_width != storage_unit_byte_width
		|| source.locator.logical_length != target_bp_size) {
		fail(ERRCODE_DATA_CORRUPTED,
			"authoritative source identity disagrees with the BatchJoin target typmod or length");
		return;
	}
	identity.table_name = format_seqidx_name(
		index_name, database_oid, tablespace_oid, main_relfilenode,
		column_attno, source.locator.bucket_index, source.locator.shard_index);
	const auto expected =
		bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
			minimizer_generation_layout::grouped_soa_segment_local,
			backend, source.alphabet_code, source.storage_unit_byte_width,
			static_cast<std::uint16_t>(params.kmer_size),
			static_cast<std::uint16_t>(params.window_size),
			bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
	if (!expected) {
		fail(ERRCODE_DATA_CORRUPTED, fmt::format(
			"GPU grouped-SoA compatibility rejected wire_error={}",
			static_cast<unsigned>(expected.error())));
		return;
	}
	auto opened = open_exact_minimizer_generation_read_session_pg(
		identity.table_name, index_name, sequence_rowno, executor_read,
		*expected);
	if (!opened) {
		const int sqlstate = opened.failure.kind
			== minimizer_generation_relation_kind::missing
			|| opened.failure.kind
				== minimizer_generation_relation_kind::legacy_exact
			? ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
			: ERRCODE_DATA_CORRUPTED;
		fail(sqlstate, fmt::format(
			"generation relation {} rejected kind={} schema_error={}",
			identity.table_name,
			static_cast<unsigned>(opened.failure.kind),
			static_cast<unsigned>(opened.failure.revision3_error)));
		return;
	}
	index_storage_fragment_generation_read_session& session = *opened.session;
	const auto head = session.read_active_head_once();
	const auto& metadata = session.read_manifest_directory_once(head);
	const auto manifest = metadata.manifest;
	session.validate_source_attestation(std::move(source_proof));

	auto& runtime = bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
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

	auto ingest = bioseq::indexing::gpu_grouped_soa_generation_ingest::begin(
		runtime, head, manifest, *expected, metadata.directory,
		cold_load_identity);
	if (!ingest) {
		fail(ERRCODE_FEATURE_NOT_SUPPORTED, fmt::format(
			"GPU grouped-SoA resident admission rejected foundation_error={}",
			static_cast<unsigned>(ingest.error())));
		return;
	}
	std::optional<gpu_grouped_soa_ingest_error> callback_error;
	std::exception_ptr callback_exception;
	const auto accept = [&](const bioseq::indexing::
			validated_minimizer_generation_row_receipt& receipt) noexcept {
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
			"GPU grouped-SoA payload ingest rejected foundation_error={}",
			static_cast<unsigned>(*callback_error)));
		return;
	}
	auto finished = ingest->finish();
	if (!finished) {
		fail(ERRCODE_DATA_CORRUPTED, fmt::format(
			"GPU grouped-SoA resident publication rejected foundation_error={}",
			static_cast<unsigned>(finished.error())));
		return;
	}
	std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target>
		resident = std::move(*finished);
	const minimizer_generation_read_stats read_stats = session.finish();
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
		&& manifest.gpu_abi_revision
			== bioseq::indexing::k_grouped_soa_gpu_abi_revision
		&& manifest.max_occ_cap
			== bioseq::indexing::k_grouped_soa_default_max_occurrence_cap
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
			"GPU grouped-SoA READY identity, read-work, upload, or source proof disagrees");
		return;
	}

	prepared_target.target_rowno = static_cast<std::int64_t>(sequence_rowno);
	prepared_target.target_bp_size = target_bp_size;
	prepared_target.query_count = query_count;
	prepared_target.persisted_read_stats = read_stats;
	prepared_target.resident = std::move(resident);
	prepared_target.resident_view =
		bioseq::gpu::vulkan::gpu_grouped_soa_resident_view{
			.descriptors = &prepared_target.resident->descriptors(),
			.controls = &prepared_target.resident->controls(),
			.positions = &prepared_target.resident->positions(),
			.descriptor_bytes = prepared_target.resident->descriptor_bytes(),
			.control_bytes = prepared_target.resident->control_bytes(),
			.position_bytes = prepared_target.resident->position_bytes(),
			.position_count = prepared_target.resident->position_count(),
			.max_occ_cap =
				bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
			.generation_id = resident_identity.generation_id,
			.source_identity = resident_identity.source_identity,
			.cold_load_identity = resident_identity.cold_load_identity,
			.partition_count = prepared_target.resident->descriptor_count(),
			.abi_revision = resident_identity.gpu_abi_revision,
			.device_uuid = resident_identity.device_uuid,
		};
	if (!prepared_target.ready()) {
		fail(ERRCODE_DATA_CORRUPTED,
			"GPU grouped-SoA prepared-target view failed its final ownership check");
	}
}

} // namespace bioseq::simd
