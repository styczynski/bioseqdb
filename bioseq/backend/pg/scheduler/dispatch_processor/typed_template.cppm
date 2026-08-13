// Attribute-driven namespace-materialization input only. This scan-only module
// supplies canonical source geometry; it is never attached to a compiler target.
module;

#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

export module bioseq.codegen.inputs.dispatch_processor_typed;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
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
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

namespace bioseq::simd::cpu_preparation_detail {

template <class p_alphabet, std::size_t p_width>
[[nodiscard]] const batch_processor::cpu_preparation_typmod_operations&
typmod_ops() noexcept;

} // namespace bioseq::simd::cpu_preparation_detail

/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor/index.cppm
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
 * 1. The active minimizer policy (`scanner_pg_context::active_index`).
 *    - Resolved once for the target column when a user relation is known.
 *      `bioseq.index.sequence_read.enabled` controls whether stored chunks are read;
 *      recompute paths still use this policy to choose transient
 *      minimizer-fragment vs minimizer-sequence storage.
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
 *      `bioseq.index.recompute_async_persistence.enabled = on`,
 *      `bioseq.index.async_persistence.enabled = on`, a user relation is known, and the
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
 * - `bioseq.index.insert_maintenance.enabled`: write/load-time policy gate. When off,
 *   INSERT/load does not build UPDATE_ON_INSERT indexes. It does not by itself
 *   change the read path except by making stored indexes absent.
 *
 * - `bioseq.index.sequence_read.enabled`: read-time gate for stored-index reuse. When
 *   on, each row tries to assemble the active stored index if chunks exist.
 *   When off, no stored minimizer index is read even if chunks are present;
 *   rows go to recompute/transient paths that still honor the active
 *   minimizer-fragment/minimizer-sequence policy shape.
 *
 * - `bioseq.index.async_persistence.enabled`: global async persistence gate. If off, async
 *   staging functions refuse to enqueue/write opportunistically built indexes.
 *   It does not pick an index by itself.
 *
 * - `bioseq.index.recompute_async_persistence.enabled`: no-index recompute gate. It allows a
 *   recompute path to consult UPDATE_ON_INSERT minimizer policy and stage or
 *   otherwise handle a deferred minimizer target, but only together with
 *   `bioseq.index.async_persistence.enabled` and only for persisted parameters `k=15,w=10`.
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
 *   If the row has minimizer-sequence chunks, assemble a query-specific index
 *   from the stored minimizer-sequence representation and search with it.
 *   Logical-fragment-backed minimizer-sequence chunks remain supported. No new
 *   persistence is staged. If chunks are
 *   missing, fall through to recompute.
 *
 * - No active read row.
 *   The resolver defaults to `minimizers_per_fragment` when reads are enabled.
 *   If `bioseq.index.sequence_read.enabled` is off, no stored index is read; the
 *   resolved policy still chooses the transient recompute shape.
 *
 * Stored-index missing or reads disabled:
 *
 * - Active read index = `minimizers_per_fragment`.
 *   Recompute the search index. If RAM-first opportunistic build is admitted,
 *   the computed fragment chunks may be staged under
 *   `minimizers_per_fragment` when `bioseq.index.async_persistence.enabled` is on.
 *   In later resident recompute paths, if deferred choice also selects
 *   `minimizers_per_fragment`, the recomputed fragment index is written to a
 *   temp payload and staged for async persistence.
 *
 * - Active read index = `minimizers_per_sequence`.
 *   First, the RAM-first opportunistic branch may build fragment-shaped chunks
 *   and stage them under the minimizer-sequence name if memory admission succeeds and
 *   `bioseq.index.async_persistence.enabled` is on. If that is not admitted, the transient
 *   no-index minimizer-sequence branch can run when the deferred recompute gates are on
 *   and the UPDATE_ON_INSERT minimizer choice is minimizer-sequence. That branch materializes
 *   the target in memory when admitted, builds query-filtered transient segment
 *   indexes, searches, and logs `persisted=false`. This is an intentional
 *   semantic exception: the GUCs are used as eligibility gates, but no durable
 *   minimizer-sequence rows are staged by that fast path.
 *
 * - No UPDATE_ON_INSERT minimizer choice.
 *   Recompute and search normally. If `bioseq.index.recompute_async_persistence.enabled` was on
 *   but no eligible minimizer policy exists, the code logs that async-persist
 *   was skipped.
 *
 *-------------------------------------------------------------------------
 */
namespace bioseq::simd {
namespace typed_dispatch {

using alphabet
    [[bioseq::template_flatten(
        "identity", "alphabet",
        "dna4", "::seq::dna4",
        "dna5", "::seq::dna5")]]
    = ::seq::dna4;

static constexpr std::size_t storage_width
    [[bioseq::template_flatten(
        "identity", "width",
        "w1", "1",
        "w2", "2",
        "w4", "4",
        "w8", "8")]]
    = 1;

static constexpr std::uint8_t compile_shard
    [[bioseq::template_flatten(
        "shard", "shape",
        "root", "0",
        "scanner_pg", "1",
        "inmemory", "2",
        "inmemory_standard", "3",
        "inmemory_segmented", "4",
        "inmemory_chunked", "5",
        "inmemory_chunked_segmented", "6")]]
    = 0;

namespace {

/// Arms before fragment-anchor preparation and completes the scratch wave only
/// after the synchronous prepare/consumer scope has unwound. The scratch API is
/// idempotent/noexcept, so preparation and alignment failures both leave the
/// reusable planner clean.
class fragment_anchor_wave_completion final {
private:
    bioseq::indexing::query_wave_scratch* scratch_ = nullptr;

public:
    explicit fragment_anchor_wave_completion(
            bioseq::indexing::query_wave_scratch& scratch) noexcept
        : scratch_(&scratch) {}

    ~fragment_anchor_wave_completion() noexcept {
        scratch_->complete_wave_after_join();
    }

    fragment_anchor_wave_completion(
        const fragment_anchor_wave_completion&) = delete;
    fragment_anchor_wave_completion& operator=(
        const fragment_anchor_wave_completion&) = delete;
};

constexpr std::string_view minimizers_per_fragment_index_name =
    bioseq::indexing::minimizer_hashmap_fragment_index::index_name;
constexpr std::string_view minimizers_per_sequence_index_name =
    bioseq::indexing::index_descriptor<
        bioseq::indexing::direct_minimizer_bucket_index>::index_name;
constexpr sequence_size_t postgres_segmentation_threshold_bp = 256'000'000;
constexpr sequence_size_t opportunistic_index_minimum_bp = 4096;

// GetConfigOptionByName returns a palloc-owned string and can ereport.  Keep the
// PostgreSQL call and release in separate PG leaves; copy only after the getter
// leaf has restored PG state so the owning std::string always unwinds normally.
[[nodiscard]] inline std::optional<std::string> pg_guc_value_as_cpp(
		const char* name) {
	char* raw_value = bioseq::postgres::support::invoke_pg_as_cpp(
		[name] { return GetConfigOptionByName(name, nullptr, true); });
	if (raw_value == nullptr) {
		return std::nullopt;
	}

	std::string value;
	try {
		value.assign(raw_value);
	} catch (...) {
		bioseq::postgres::support::invoke_pg_as_cpp(
			[raw_value] { pfree(raw_value); });
		throw;
	}
	bioseq::postgres::support::invoke_pg_as_cpp(
		[raw_value] { pfree(raw_value); });
	return value;
}

[[nodiscard]] inline bool pg_vulkan_seed_lookup_selected(
        const minimap2_parameters& params) noexcept
{
    return params.vulkan_enabled && params.vulkan_seed_hits_enabled;
}

[[nodiscard]] inline std::uint64_t pg_gpu_index_resident_limit_bytes()
{
    bioseq::gpu::vulkan::runtime& runtime =
        bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
    std::uint64_t limit = runtime.is_usable()
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

[[nodiscard]] inline bioseq::indexing::gpu_index_cache_identity
pg_gpu_index_identity(
        std::uint64_t rowno,
        std::string_view index_name,
        sequence_size_t clip_lo_bp,
        sequence_size_t clip_hi_bp,
        bioseq::minimizers::minimizer_sketch_backend sketch_backend) noexcept
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
        static_cast<std::uint64_t>(scheme.revision) + (h << 1));
    if (h == 0) {
        h = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::gpu_index_cache_identity{.assembly_identity = h};
}

[[nodiscard]] inline bioseq::indexing::gpu_index_cache_identity
pg_gpu_index_identity(
        const bioseq::storage::indexing::assembled_index_key& key) noexcept
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
        static_cast<std::uint64_t>(key.minimizer_scheme.revision) + (h << 1));
    h ^= bioseq::indexing::splitmix64(key.query_key + (h << 1));
    h ^= bioseq::indexing::splitmix64(key.query_signature + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_lo_bp) + (h << 1));
    h ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_hi_bp) + (h << 1));
    if (h == 0) {
        h = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::gpu_index_cache_identity{.assembly_identity = h};
}

enum class deferred_minimizer_index_kind : std::uint8_t {
    per_fragment,
    per_sequence,
};

struct deferred_minimizer_persist_choice {
    deferred_minimizer_index_kind kind = deferred_minimizer_index_kind::per_fragment;
    std::string_view index_name = minimizers_per_fragment_index_name;
};

[[nodiscard]] deferred_minimizer_index_kind deferred_kind_for_index(
        std::string_view index_name) noexcept {
    return index_name == minimizers_per_sequence_index_name
        ? deferred_minimizer_index_kind::per_sequence
        : deferred_minimizer_index_kind::per_fragment;
}

[[nodiscard]] std::optional<deferred_minimizer_persist_choice>
choose_deferred_minimizer_persist_index(
        const batch_processor::scanner_pg_context& ctx) {
    if (!OidIsValid(ctx.user_relid) || ctx.mmsearch_seq_attno <= 0) {
        return std::nullopt;
    }

    constexpr auto minimizers = bioseq::indexing::k_minimizer_index_names;
    const auto policy = bioseq::storage::indexing::
        resolve_minimizer_persistence_policy(
            ctx.user_relid, ctx.main_relfilenode,
            ctx.mmsearch_seq_attno, ctx.snapshot);

    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        if (policy.enabled[i] && policy.update_on_insert[i]) {
            return deferred_minimizer_persist_choice{
                .kind = deferred_kind_for_index(minimizers[i]),
                .index_name = minimizers[i],
            };
        }
    }
    return std::nullopt;
}


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
[[nodiscard]] inline minimap2_parameters apply_size_heuristic(
    const minimap2_parameters& base, size_t min_bp)
{
    return batch_processor::effective_params_for_pair(base, min_bp);
}

[[nodiscard]] inline bool selected_vulkan_execution(
        const minimap2_parameters& params) noexcept {
    return pipe::vulkan_components_requested(params);
}

[[nodiscard]] inline bool selected_vulkan_accelerator(
        const minimap2_parameters& params) noexcept {
    return pipe::vulkan_accelerator_requested(params);
}

[[nodiscard]] inline bool selected_vulkan_coordinate_prereqs_active(
        const minimap2_parameters& params) noexcept {
    return pipe::complete_all_gpu_tuple_selected(params);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::executed_backend
backend_from_selected_params(const minimap2_parameters& params) noexcept {
    const bool full_vulkan_pipeline =
        pipe::complete_all_gpu_tuple_selected(params);
    if (full_vulkan_pipeline) {
        return ::bioseq::pipeline_stats::executed_backend::vulkan_gpu;
    }
    if (selected_vulkan_execution(params)) {
        return ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash
        && params.chaining_algorithm == chaining_algorithm::simd_minimap2
        && params.base_alignment_algorithm
            == base_alignment_algorithm::block_aligner) {
        return ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    }
    if (params.minimizer_algorithm
            == minimizer_algorithm::bounded_nthash_simd
        && params.chaining_algorithm == chaining_algorithm::simd_minimap2
        && params.base_alignment_algorithm
            == base_alignment_algorithm::block_aligner) {
        return ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::scalar
        && params.chaining_algorithm == chaining_algorithm::scalar_minimap2
        && params.base_alignment_algorithm == base_alignment_algorithm::ksw2) {
        return ::bioseq::pipeline_stats::executed_backend::scalar_cpu;
    }
    return ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_chaining_policy_identity
requested_simd_chaining_policy_identity(
        const minimap2_parameters& params) noexcept {
    using identity_type =
        ::bioseq::pipeline_stats::simd_chaining_policy_identity;
    if (params.chaining_algorithm != chaining_algorithm::simd_minimap2) {
        return identity_type::none;
    }
    switch (params.simd_chaining_policy) {
        case simd_chaining_policy::exact_visited:
            return identity_type::exact_visited;
        case simd_chaining_policy::bounded_batch_approximate:
            return identity_type::bounded_batch_approximate;
    }
    return identity_type::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_chaining_policy_identity
observed_simd_chaining_policy_identity(
        const match_result_collection* execution_stats) noexcept {
    using identity_type =
        ::bioseq::pipeline_stats::simd_chaining_policy_identity;
    if (execution_stats == nullptr
        || !execution_stats->simd_chaining_execution_stats_available) {
        return identity_type::none;
    }
    const auto& policy =
        execution_stats->simd_chaining_stats.policy;
    // identity_type answers only which named policy emitted operation calls.
    // Rejections, saturation, forbidden scalar work, and missing vector work
    // are retained as separate fail-closed validation stats below; folding them into
    // identity would mislabel a rejected exact (or bounded) execution as a
    // mixed-policy execution and hide the actual reason for rejection.
    const bool exact = policy.exact_visited_calls != 0;
    const bool bounded = policy.bounded_batch_approximate_calls != 0;
    if (exact == bounded) {
        // Neither identity, or an impossible validation accepting both identities,
        // is not a policy absence. Preserve it as mixed-invalid state.
        return identity_type::mixed_invalid;
    }
    return exact ? identity_type::exact_visited
                 : identity_type::bounded_batch_approximate;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_alignment_algorithm_identity
requested_simd_alignment_algorithm_identity(
        const minimap2_parameters& params) noexcept {
    using identity_type =
        ::bioseq::pipeline_stats::simd_alignment_algorithm_identity;
    return backend_from_selected_params(params)
            == ::bioseq::pipeline_stats::executed_backend::simd_cpu
        ? identity_type::block_aligner
        : identity_type::none;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_alignment_algorithm_identity
observed_simd_alignment_algorithm_identity(
        const match_result_collection* execution_stats) noexcept {
    using identity_type =
        ::bioseq::pipeline_stats::simd_alignment_algorithm_identity;
    if (execution_stats == nullptr) return identity_type::none;

    const bool block_present =
        execution_stats->block_aligner_execution_stats_available;
    const auto& block = execution_stats->block_aligner_stats;
    const auto& scalar = execution_stats->scalar_pipeline_stats;
    const bool scalar_alignment_observed =
        execution_stats->scalar_pipeline_execution_stats_available
        && (scalar.ksw2_left_calls != 0
            || scalar.ksw2_gap_calls != 0
            || scalar.ksw2_right_calls != 0);
    const bool forbidden_alignment_observed = scalar_alignment_observed
        || execution_stats->ksw2_batched_execution_stats_available
        || (block_present
            && (block.forbidden_ksw2_calls != 0
                || block.forbidden_scalar_biological_fallbacks != 0));
    if (block_present && forbidden_alignment_observed) {
        return identity_type::mixed_invalid;
    }
    return block_present ? identity_type::block_aligner : identity_type::none;
}

inline void add_operational_counter(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        std::uint64_t& destination,
        const std::uint64_t increment = 1) noexcept {
    stats.operational.are_stats_saturated =
        ::bioseq::pipeline_stats::merge_counter(destination, increment)
        || stats.operational.are_stats_saturated;
}

inline void add_stage_host_time(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        const ::bioseq::pipeline_stats::pipeline_stage stage,
        const std::uint64_t elapsed_ns) noexcept {
    auto& destination = stats.operational.timing[
        ::bioseq::pipeline_stats::stage_index(stage)].host_wall_ns;
    add_operational_counter(stats, destination, elapsed_ns);
}

[[nodiscard]] inline std::uint64_t dispatch_host_now_ns() noexcept {
    const auto now = bioseq::instrumentation::clock::now_ns();
    return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
}

[[nodiscard]] inline std::uint64_t elapsed_dispatch_host_ns(
        const std::uint64_t started_ns) noexcept {
    const std::uint64_t finished_ns = dispatch_host_now_ns();
    const std::uint64_t elapsed_ns = finished_ns >= started_ns
        ? finished_ns - started_ns : 0u;
    return elapsed_ns > 0 ? elapsed_ns : 1u;
}

inline void record_block_aligner_execution_stats(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        const match_result_collection* execution_stats) noexcept {
    if (execution_stats == nullptr
        || !execution_stats->block_aligner_execution_stats_available) {
        return;
    }
    const auto& block_stats = execution_stats->block_aligner_stats;
    ::bioseq::pipeline_stats::block_aligner_stage_stats stage_stats{};
    stage_stats.is_available = true;
    stage_stats.is_accepted = block_stats.is_internally_consistent();
    stage_stats.executed_calls = block_stats.executed_calls();
    stage_stats.nonempty_calls = block_stats.nonempty_calls;
    stage_stats.nonempty_calls_with_simd = block_stats.nonempty_calls_with_simd;
    stage_stats.simd_dp_vector_batches = block_stats.simd_dp_vector_batches;
    stage_stats.simd_dp_cells = block_stats.simd_dp_cells;
    stage_stats.forbidden_ksw2_calls = block_stats.forbidden_ksw2_calls;
    stage_stats.forbidden_scalar_biological_fallbacks =
        block_stats.forbidden_scalar_biological_fallbacks;
    stage_stats.is_saturated = block_stats.is_saturated;
    stats.operational.are_stats_saturated =
        stats.operational.block_aligner_stats.merge(stage_stats)
        || stats.operational.are_stats_saturated;
}

inline void record_unattested_job(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        const minimap2_parameters& params,
        const ::bioseq::pipeline_stats::target_index_source requested_source,
        const ::bioseq::pipeline_stats::target_index_source actual_source,
        const batch_processor::scanner_pg_context* scanner_pg_ctx,
        const std::size_t row_index,
        const bool per_row_query_mode,
        const std::uint64_t broadcast_query_ordinal,
        const match_result_collection* execution_stats,
        const bool consumed_shared_cached_query_stats,
        bool& cached_query_minimizer_stats_accounted) noexcept {
    if (execution_stats != nullptr) {
        if (execution_stats->prefilter_execution_stats_available) {
            stats.operational.record_prefilter_execution_stats(
                execution_stats->prefilter_stats);
        } else if (execution_stats->prefilter_stats
                   != decltype(execution_stats->prefilter_stats){}) {
            auto malformed_prefilter_stats =
                execution_stats->prefilter_stats;
            malformed_prefilter_stats.is_saturated = true;
            stats.operational.record_prefilter_execution_stats(
                malformed_prefilter_stats);
        }
        // CPU/SIMD stages write into the operation-local result collection;
        // consume that delta once here, after any parallel row has joined.
        // chain_stage is intentionally inclusive, so chain_backtrack remains
        // zero. The left_extension timing slot likewise owns the coarse
        // alignment-through-result interval; gap/right remain available for a
        // later measured split. These owners never overlap.
        // query_sketch is only the per-operation sketch recorded below or by
        // minimizer_stage; the batch join query-union preparation that precedes
        // persisted payload delivery has a different lifetime owner and must
        // be published separately rather than charged here twice.
        using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
        add_stage_host_time(
            stats, stage_type::query_sketch,
            execution_stats->query_sketch_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::persisted_index_decode,
            execution_stats->persisted_index_decode_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::seed_lookup,
            execution_stats->seed_lookup_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::chain_score,
            execution_stats->chain_score_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::region_generation,
            execution_stats->region_generation_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::left_extension,
            execution_stats->alignment_result_host_wall_ns);
        stats.operational.are_stats_saturated =
            stats.operational.are_stats_saturated
            || execution_stats->is_host_stage_timing_saturated;
        if (execution_stats->fragment_native_query_stats_available) {
            stats.operational.fragment_native_query_stats.merge(
                execution_stats->fragment_native_query_stats);
            stats.operational.are_stats_saturated =
                stats.operational.are_stats_saturated
                || stats.operational.fragment_native_query_stats.saturated;
        }
    }
    ::bioseq::pipeline_stats::pipeline_job_execution_stats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_simd_chaining_policy =
        requested_simd_chaining_policy_identity(params);
    job.observed_simd_chaining_policy =
        observed_simd_chaining_policy_identity(execution_stats);
    job.requested_simd_alignment_algorithm =
        requested_simd_alignment_algorithm_identity(params);
    job.observed_simd_alignment_algorithm =
        observed_simd_alignment_algorithm_identity(execution_stats);
    const bool selected_all_simd =
        job.requested_backend
            == ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    const bool selected_all_scalar =
        job.requested_backend
            == ::bioseq::pipeline_stats::executed_backend::scalar_cpu;
    const bool require_target_sketch =
        actual_source
            == ::bioseq::pipeline_stats::target_index_source::recomputed;
    const bool accepted_all_simd = selected_all_simd
        && execution_stats != nullptr
        && execution_stats->is_all_simd_execution_consistent(
            params.minimizer_algorithm,
            params.simd_chaining_policy,
            require_target_sketch,
            /*require_nonempty_vector_work=*/true);
    const bool accepted_all_scalar = selected_all_scalar
        && execution_stats != nullptr
        && execution_stats->is_all_scalar_execution_consistent(
            require_target_sketch,
            /*require_alignment=*/params.do_cigar);
    if (execution_stats != nullptr
        && execution_stats->simd_chaining_execution_stats_available) {
        const auto& policy =
            execution_stats->simd_chaining_stats.policy;
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
        stats.operational.are_simd_chaining_policy_stats_saturated =
            stats.operational.are_simd_chaining_policy_stats_saturated
            || policy.saturated;
    }
    record_block_aligner_execution_stats(stats, execution_stats);
    const auto record_exact_nthash_vector_activity = [&stats](
            const auto& range_stats) noexcept {
        // Continuation batches and whole-range resident vector_stream calls
        // remain separate stats. A resident call is authentic vector work,
        // but it is not one continuation batch.
        add_operational_counter(
            stats,
            stats.operational.nthash_vector_batches,
            range_stats.vector_batches);
        add_operational_counter(
            stats,
            stats.operational.nthash_resident_vector_stream_kernel_calls,
            range_stats.resident_vector_stream_kernel_calls);
    };
    // A broadcast persisted-query sketch is one physical operation shared by
    // logical jobs. Account it at its first validation-approved consumer even when
    // that job later has no chain/alignment; downstream route attestation is
    // a separate question. Recompute rows cannot enter this block because
    // their consumed_shared_cached_query_stats flag is false.
    const bool accepted_shared_query_sketch =
        consumed_shared_cached_query_stats
        && !cached_query_minimizer_stats_accounted
        && execution_stats != nullptr
        && (selected_all_simd
            ? execution_stats->is_selected_simd_sketch_execution_consistent(
                params.minimizer_algorithm,
                /*require_target_sketch=*/false,
                /*require_nonempty_vector_work=*/false)
            : selected_all_scalar
                && execution_stats->is_selected_scalar_sketch_execution_consistent(
                    /*require_target_sketch=*/false,
                    /*require_nonempty_work=*/false));
    if (accepted_shared_query_sketch) {
        if (params.minimizer_algorithm
                == minimizer_algorithm::two_stack_nthash) {
            record_exact_nthash_vector_activity(
                execution_stats->exact_simd_query_execution_stats);
        } else if (params.minimizer_algorithm
                    == minimizer_algorithm::bounded_nthash_simd) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                execution_stats->bounded_nthash_query_execution_stats
                    .packed_decode_batches);
        }
        cached_query_minimizer_stats_accounted = true;
    }
    // A SIMD identity is published only after the per-job collection proves
    // two_stack_nthash sketching, local SIMD score/backtracking, and Block alignment.
    // Every partial, missing, or inconsistent validation remains mixed-invalid.
    job.actual_backend = accepted_all_simd
        ? ::bioseq::pipeline_stats::executed_backend::simd_cpu
        : accepted_all_scalar
            ? ::bioseq::pipeline_stats::executed_backend::scalar_cpu
            : ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
    job.requested_target_index_source = requested_source;
    job.actual_target_index_source = actual_source;
	if (actual_source
			== ::bioseq::pipeline_stats::target_index_source::persisted
		&& scanner_pg_ctx != nullptr) {
		job.persisted_generation_id =
			scanner_pg_ctx->persisted_generation_ids[row_index];
		job.persisted_source_identity =
			scanner_pg_ctx->persisted_source_identities[row_index];
	}
    const std::size_t effective_workers =
        bioseq::parallelism::global_pool::instance().maximum_concurrency();
    job.environment.effective_cpu_workers = effective_workers
            > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())
        ? (std::numeric_limits<std::uint32_t>::max)()
        : static_cast<std::uint32_t>(effective_workers);
    job.environment.record_highway_target(
        params.highway_dispatch,
        params.highway_executed_target);
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
                ::bioseq::pipeline_stats::pipeline_stage stage,
                ::bioseq::pipeline_stats::execution_engine engine) noexcept {
            auto& coverage = job.stages.coverage[
                ::bioseq::pipeline_stats::stage_index(stage)];
            coverage.eligible_jobs = 1;
            coverage.executed_jobs[
                ::bioseq::pipeline_stats::engine_index(engine)] = 1;
        };
        using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
        using engine_type = ::bioseq::pipeline_stats::execution_engine;
        const engine_type biological_engine = accepted_all_scalar
            ? engine_type::scalar_cpu
            : engine_type::simd_cpu;
        mark(stage_type::query_sketch, biological_engine);
        if (require_target_sketch) {
            mark(stage_type::target_sketch, biological_engine);
            mark(stage_type::target_index_build, engine_type::scalar_cpu);
		} else {
			mark(stage_type::persisted_index_decode, engine_type::host_io);
			mark(stage_type::partition_selection, engine_type::scalar_cpu);
        }
        mark(stage_type::seed_lookup, engine_type::scalar_cpu);
        mark(stage_type::chain_score, biological_engine);
        mark(stage_type::chain_backtrack, biological_engine);
        if (accepted_all_simd) {
            const auto& block = execution_stats->block_aligner_stats;
            if (block.left_executed != 0) {
                mark(stage_type::left_extension, engine_type::simd_cpu);
            }
            if (block.gap_executed != 0) {
                mark(stage_type::gap_extension, engine_type::simd_cpu);
            }
            if (block.right_executed != 0) {
                mark(stage_type::right_extension, engine_type::simd_cpu);
            }
        } else {
            const auto& scalar =
                execution_stats->scalar_pipeline_stats;
            if (scalar.ksw2_left_calls != 0) {
                mark(stage_type::left_extension, engine_type::scalar_cpu);
            }
            if (scalar.ksw2_gap_calls != 0) {
                mark(stage_type::gap_extension, engine_type::scalar_cpu);
            }
            if (scalar.ksw2_right_calls != 0) {
                mark(stage_type::right_extension, engine_type::scalar_cpu);
            }
        }

        if (accepted_all_simd
            && params.minimizer_algorithm
                == minimizer_algorithm::two_stack_nthash) {
            if (!consumed_shared_cached_query_stats) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_query_execution_stats);
            }
            if (execution_stats->exact_simd_target_execution_stats_available) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_target_execution_stats);
            }
        } else if (accepted_all_simd
                   && params.minimizer_algorithm
                    == minimizer_algorithm::bounded_nthash_simd) {
            if (!consumed_shared_cached_query_stats) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_stats->bounded_nthash_query_execution_stats
                        .packed_decode_batches);
            }
            if (execution_stats->bounded_nthash_target_execution_stats_available) {
                add_operational_counter(
                    stats, stats.operational.nthash_vector_batches,
                    execution_stats->bounded_nthash_target_execution_stats
                        .packed_decode_batches);
            }
        }
        if (accepted_all_simd) {
            add_operational_counter(
                stats, stats.operational.simd_chain_vector_batches,
                execution_stats->simd_chaining_stats
                    .score_vector_batches);
            add_operational_counter(
                stats,
                stats.operational.simd_chain_backtrack_vector_batches,
                execution_stats->simd_chaining_stats
                    .backtrack_path_vector_batches);
        }
    }
    // Unsupported tuples and incomplete scalar/SIMD paths deliberately remain
    // unattested. Persisted identity remains fail-closed until its reader
    // publishes the generation validation.
    stats.merge_job(job);
}

/// Consume one runtime-sealed persisted grouped-SoA coordinate receipt into a local
/// job value.  Nothing caller-visible is mutated here: a missing, tampered or
/// repeated consume burns only the runtime record and returns no staged job.
/// The accepted operation authority supplies the immutable GPU environment
/// after every job and the complete causal receipt union have been validated.
[[nodiscard]] inline std::optional<
    ::bioseq::pipeline_stats::pipeline_job_execution_stats>
stage_persisted_grouped_soa_coordinate_job(
        const minimap2_parameters& params,
        const ::bioseq::pipeline_stats::target_index_source requested_source,
        bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_operation_receipt&
            receipt,
		const bioseq::gpu::vulkan::
			persisted_grouped_soa_coordinate_job_expectation& expectation) noexcept {
    ::bioseq::pipeline_stats::pipeline_job_execution_stats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_target_index_source = requested_source;
    const auto consumed_validation = receipt.consume(expectation);
    const bool accepted =
        job.requested_backend
            == ::bioseq::pipeline_stats::executed_backend::vulkan_gpu
        && selected_vulkan_execution(params)
        && !params.do_cigar
		&& pipe::is_gpu_coordinate_only_execution_active(params)
		&& pipe::is_gpu_target_index_build_active(params)
        && requested_source
            == ::bioseq::pipeline_stats::target_index_source::persisted
        && consumed_validation.has_value()
        && consumed_validation->is_valid();
    if (!accepted) return std::nullopt;
    job.actual_backend = ::bioseq::pipeline_stats::executed_backend::vulkan_gpu;
    job.actual_target_index_source =
        ::bioseq::pipeline_stats::target_index_source::persisted;
    job.persisted_generation_id = consumed_validation->audit().generation_id;
    job.persisted_source_identity = consumed_validation->audit().source_identity;
    job.logical_target_row = expectation.logical_target_row;
    job.query_ordinal = expectation.logical_query_ordinal;
    const auto mark = [&job](
            const ::bioseq::pipeline_stats::pipeline_stage stage,
            const ::bioseq::pipeline_stats::execution_engine engine) {
        auto& coverage = job.stages.coverage[
            ::bioseq::pipeline_stats::stage_index(stage)];
        coverage.eligible_jobs = 1u;
        coverage.executed_jobs[
            ::bioseq::pipeline_stats::engine_index(engine)] = 1u;
    };
    using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
    using engine_type = ::bioseq::pipeline_stats::execution_engine;
    mark(stage_type::query_sketch, engine_type::vulkan_gpu);
    mark(stage_type::persisted_index_decode, engine_type::host_io);
    if (consumed_validation->audit().query_record.minimizer_count != 0u) {
        mark(stage_type::partition_selection, engine_type::vulkan_gpu);
        mark(stage_type::seed_lookup, engine_type::vulkan_gpu);
    }
    if (consumed_validation->audit().range_count != 0u) {
        mark(stage_type::chain_score, engine_type::vulkan_gpu);
        mark(stage_type::chain_backtrack, engine_type::vulkan_gpu);
        mark(stage_type::region_generation, engine_type::vulkan_gpu);
    }
    return job;
}

/// Copy one runtime cache audit into the pointer-free SQL ledger wire.
[[nodiscard]] inline ::bioseq::pipeline_stats::
persisted_gpu_cache_metrics
make_persisted_gpu_cache_metrics_stats(
        const bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& source)
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

template <std::size_t p_capacity>
[[nodiscard]] inline std::uint32_t copy_persisted_gpu_stats_text(
        std::array<char, p_capacity>& destination,
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
persisted_gpu_cache_validation_result
make_persisted_gpu_cache_validation_stats(
        const bioseq::gpu::vulkan::
            runtime_metal_archive_process_qualification& source) noexcept {
    ::bioseq::pipeline_stats::persisted_gpu_cache_validation_result result{
        .qualification_wire_revision = source.wire_revision,
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
        .helper_wire_revision = source.helper_wire_revision,
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
        .is_saturated = source.saturated,
    };
    result.helper_build_identity_size = copy_persisted_gpu_stats_text(
        result.helper_build_identity, source.helper_build_identity);
    result.helper_source_size = copy_persisted_gpu_stats_text(
        result.helper_source, source.helper_source);
    return result;
}

/// Copy one full-stage runtime job preview into the admitted ledger slot.
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_job_stats
make_persisted_gpu_job_stats(
        const bioseq::gpu::vulkan::
            persisted_grouped_soa_coordinate_job_receipt_preview& source) noexcept {
    const auto& audit = source.audit;
    const auto& coordinate = audit.result;
    const std::array<std::uint64_t, 4> submit_wait_identities{
        audit.count_submit_wait_identity,
        audit.emit_submit_wait_identity,
        audit.sort_range_submit_wait_identity,
        audit.chain_coordinate_submit_wait_identity,
    };
    ::bioseq::pipeline_stats::persisted_gpu_job_stats result{
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
            .is_valid = coordinate.is_valid != 0u,
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
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_operation_stats
make_persisted_gpu_operation_stats(
        const bioseq::gpu::vulkan::
            persisted_grouped_soa_coordinate_operation_receipt_preview& source,
        const ::bioseq::pipeline_stats::
            persisted_grouped_soa_coordinate_publication_stats& publication,
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
            make_persisted_gpu_cache_validation_stats(
                source.cache_qualification),
        .cache_metrics = make_persisted_gpu_cache_metrics_stats(
            source.maximum_cache_metrics),
    };
}

/// Conservative admission envelope for the N-job capacities that coexist
/// between persisted grouped-SoA execution and atomic SQL publication.  The
/// returned reservation covers transient stats/staging overlap; the result map
/// that leaves process_sequences_typed() remains part of the pre-existing
/// caller-owned output contract.
[[nodiscard]] inline std::uint64_t
persisted_grouped_soa_publication_staging_bytes(
        const std::uint64_t job_count) noexcept {
    using staged_job_type = ::bioseq::pipeline_stats::pipeline_job_execution_stats;
    using result_map_type = std::map<int32_t, std::vector<match_result>>;
    using result_map_value_type = typename result_map_type::value_type;

    std::uint64_t bytes_per_job = 0u;
    const auto add = [&bytes_per_job](const std::uint64_t bytes) noexcept {
        bytes_per_job = bioseq::common::saturating_add(
            bytes_per_job, bytes);
    };
    add(sizeof(bioseq::gpu::vulkan::gpu_seed_coordinate_job));
    add(sizeof(std::size_t));
    add(sizeof(bioseq::gpu::vulkan::gpu_coordinate_result_wire));
    // Runtime owns separate retained permits for stats records and both
    // causal audit-job vectors; counting them here would double-reserve them.
    add(sizeof(staged_job_type));
    // At most one ordered-map node per job. Four pointers cover
    // parent/left/right plus allocator/alignment bookkeeping.
    add(sizeof(result_map_value_type));
    add(4u * sizeof(void*));
    // Repeated query ids can geometrically grow result vectors, but total
    // capacity remains below two elements per appended coordinate result.
    add(bioseq::common::saturating_mul(2u, sizeof(match_result)));

    return bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(job_count, bytes_per_job),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

inline void record_target_row(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        const std::uint64_t rows = 1) noexcept {
    add_operational_counter(stats, stats.operational.target_rows, rows);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::target_index_source
requested_target_index_source(
        const batch_processor::scanner_pg_context* scanner_pg_ctx) noexcept {
    if (scanner_pg_ctx == nullptr) {
        return ::bioseq::pipeline_stats::target_index_source::recomputed;
    }
    switch (scanner_pg_ctx->index_read_requirement) {
        case bioseq::indexing::index_read_requirement::require_absent:
            return ::bioseq::pipeline_stats::target_index_source::recomputed;
        case bioseq::indexing::index_read_requirement::
                require_compatible_persisted:
            return ::bioseq::pipeline_stats::target_index_source::persisted;
        case bioseq::indexing::index_read_requirement::allow:
            return scanner_pg_ctx->read_index_active
                ? ::bioseq::pipeline_stats::target_index_source::persisted
                : ::bioseq::pipeline_stats::target_index_source::recomputed;
    }
    return ::bioseq::pipeline_stats::target_index_source::recomputed;
}

// PostgreSQL ERROR is implemented with longjmp.  Keep diagnostic formatting and
// every owning C++ object in the ordinary C++ caller, and enter PostgreSQL only
// through this primitive-capture leaf.  invoke_pg_as_cpp converts the ERROR to a
// postgres_error after leaving PG_TRY, so normal C++ unwinding owns all
// destruction.
[[noreturn]] inline void raise_typed_dispatch_pg_error_as_cpp(
		const int sqlstate,
		const char* message,
		const char* detail = nullptr,
		const char* hint = nullptr) {
	bioseq::postgres::support::invoke_pg_as_cpp(
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
        const batch_processor::scanner_pg_context* scanner_pg_ctx,
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
		"Publish and wire a validated current READY generation reader, "
		"or choose index_read_requirement=allow/require_absent; pair-sort "
		"rebuild and recompute fallbacks are forbidden.");
}

inline void record_target_index_outcome(
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        const ::bioseq::pipeline_stats::target_index_source requested_source,
        const ::bioseq::pipeline_stats::target_index_source actual_source,
        const std::uint64_t rows = 1) noexcept {
    if (actual_source
        == ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(
            stats, stats.operational.persisted_index_rows, rows);
    } else if (actual_source
               == ::bioseq::pipeline_stats::target_index_source::recomputed) {
        add_operational_counter(
            stats, stats.operational.recomputed_target_index_rows, rows);
    }
    if (requested_source
            == ::bioseq::pipeline_stats::target_index_source::persisted
        && actual_source
            != ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(
            stats, stats.operational.missing_persisted_index_rows, rows);
    }
}

template <class p_sequence>
[[nodiscard]] std::span<const std::uint32_t> append_gpu_alignment_symbols(
        std::vector<std::uint32_t>& destination,
        std::vector<typename p_sequence::traits::letter_unit>& scratch,
        const p_sequence& seq,
        sequence_size_t start_bp,
        sequence_size_t end_bp,
        bool reverse)
{
    using traits = typename p_sequence::traits;
    using rank_t = typename traits::letter_unit;

    if (end_bp < start_bp) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR extension received inverted sequence window");
    }
    const sequence_size_t len_bp = end_bp - start_bp;
    if (len_bp > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "GPU CIGAR extension window length {} exceeds simple CIGAR limit {}",
            len_bp, bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len));
    }
    if (end_bp > static_cast<sequence_size_t>(seq.bp_size())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
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
            seq.template copy_ranks<false, true>(out8, start, end);
            if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                for (rank_t& rank : scratch) {
                    rank = static_cast<rank_t>(
                        traits::letter_complement(
                            static_cast<typename traits::storage_unit>(rank)));
                }
            }
        } else {
            seq.template copy_ranks<false, false>(out8, start, end);
        }
        for (std::size_t i = 0; i < len; ++i) {
            destination[out_offset + i] = static_cast<std::uint32_t>(scratch[i]);
        }
    } else {
        auto iter = seq.template iterable<logical_rank>().begin();
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

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::span<const std::uint32_t> append_gpu_alignment_symbols(
        std::vector<std::uint32_t>& destination,
        std::vector<std::uint8_t>& scratch,
        const bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        sequence_size_t start_bp,
        sequence_size_t end_bp,
        bool reverse) {
    using traits = bioseq::storage::lazy_sequence_traits<p_alphabet>;
    if (end_bp < start_bp || end_bp > sequence.bp_size()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR lazy sequence window is invalid");
    }
    const sequence_size_t length_bp = end_bp - start_bp;
    if (length_bp
        > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR lazy sequence window exceeds the supported limit");
    }
    const std::size_t length = static_cast<std::size_t>(length_bp);
    const std::size_t output_offset = destination.size();
    destination.resize(output_offset + length);
    scratch.resize(length);
    if (length != 0u) {
        auto encoded = std::span<std::uint8_t>{scratch.data(), length};
        if (reverse) {
            sequence.template copy_ranks<false, true>(
                encoded,
                static_cast<std::size_t>(start_bp),
                static_cast<std::size_t>(end_bp));
            if constexpr (seqan3::nucleotide_alphabet<p_alphabet>) {
                for (std::uint8_t& rank : scratch) {
                    rank = traits::letter_complement(rank);
                }
            }
        } else {
            sequence.template copy_ranks<false, false>(
                encoded,
                static_cast<std::size_t>(start_bp),
                static_cast<std::size_t>(end_bp));
        }
        for (std::size_t index = 0u; index < length; ++index) {
            destination[output_offset + index] = scratch[index];
        }
    }
    return std::span<const std::uint32_t>{
        destination.data() + output_offset, length};
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
        std::vector<cigar_operation>& destination,
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
            destination.push_back(cigar_operation{op_length, opcode});
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

[[noreturn]] inline void raise_bounded_nthash_pg_error(
        std::string_view stage,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	raise_typed_dispatch_pg_error_as_cpp(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		"Bounded ntHash SIMD minimizer execution is unsupported for this request",
		diagnostic.c_str(),
			"Bounded ntHash SIMD has no scalar or Exact SIMD fallback; use "
			"DNA4 with packed uint32/uint64 storage, W=10, K=15, and an "
			"admissible range, or select another algorithm explicitly.");
}

[[noreturn]] inline void raise_simd_chaining_pg_error(
        std::string_view stage,
        pipe::simd_chaining_status status,
        std::string_view detail) {
	const std::string diagnostic = fmt::format("{}: {}", stage, detail);
	if (status == pipe::simd_chaining_status::invalid_policy) {
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

template <std::size_t p_capacity>
[[nodiscard]] inline std::size_t copy_pg_error_detail(
        std::array<char, p_capacity>& destination,
        const char* source) noexcept {
    static_assert(p_capacity > 0u);
    if (source == nullptr) {
        destination[0] = '\0';
        return 0u;
    }
    std::size_t size = 0u;
    while (size + 1u < p_capacity && source[size] != '\0') {
        destination[size] = source[size];
        ++size;
    }
    destination[size] = '\0';
    return size;
}

template <class p_function>
[[nodiscard]] match_result_collection run_aligner_pg_checked(
        std::string_view stage,
        p_function&& fn) {
    enum class failure_kind : std::uint8_t {
        none,
        vulkan,
        exact_simd,
        bounded_nthash,
        simd_chaining,
        persisted_minimizer_lookup,
    };
    failure_kind failure = failure_kind::none;
    pipe::simd_chaining_status simd_chaining_status =
        pipe::simd_chaining_status::ok;
    // PostgreSQL ERROR is a longjmp. Keep only trivially destructible state
    // alive after the C++ catch blocks; a std::string here would have its
    // destructor skipped by every raise_*_pg_error call below.
    std::array<char, 2048> detail{};
    std::size_t detail_size = 0u;
    try {
        return std::forward<p_function>(fn)();
    } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
        failure = failure_kind::vulkan;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::exact_simd_runtime_error& err) {
        failure = failure_kind::exact_simd;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::bounded_nthash_runtime_error& err) {
        failure = failure_kind::bounded_nthash;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::simd_chaining_runtime_error& err) {
        failure = failure_kind::simd_chaining;
        simd_chaining_status = err.status();
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const bioseq::indexing::persisted_minimizer_lookup_error& err) {
        failure = failure_kind::persisted_minimizer_lookup;
        detail_size = copy_pg_error_detail(detail, err.what());
    }
    // The C++ exception object has been destroyed, and only the fixed buffer,
    // length, enum, and string_view arguments remain across PostgreSQL ERROR.
	const std::string_view detail_view{detail.data(), detail_size};
	switch (failure) {
		case failure_kind::vulkan:
			raise_vulkan_pg_error(stage, detail_view);
		case failure_kind::exact_simd:
			raise_exact_simd_pg_error(stage, detail_view);
		case failure_kind::bounded_nthash:
			raise_bounded_nthash_pg_error(stage, detail_view);
		case failure_kind::simd_chaining:
			raise_simd_chaining_pg_error(
				stage, simd_chaining_status, detail_view);
		case failure_kind::persisted_minimizer_lookup:
			raise_persisted_minimizer_lookup_pg_error(stage, detail_view);
		case failure_kind::none:
			break;
	}
    __builtin_unreachable();
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_raw_pg_checked(
        p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap_port",
        [&args...]() -> match_result_collection {
            return minimap_port_raw_owned(
                std::forward<p_arguments>(args)...);
        });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_segmented_pg_checked(
        p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap_port_segmented",
        [&args...]() -> match_result_collection {
            return minimap_port_segmented_owned(
                std::forward<p_arguments>(args)...);
        });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_fragment_index_vector_pg_checked(
        p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index",
        [&args...]() -> match_result_collection {
            return minimap_port_fragment_index_vector_owned(
                std::forward<p_arguments>(args)...);
		});
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_hash_major_pg_checked(
        p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index",
        [&args...]() -> match_result_collection {
            return minimap_port_hash_major_owned(
                std::forward<p_arguments>(args)...);
		});
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_grouped_soa_pg_checked(
        p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index",
        [&args...]() -> match_result_collection {
            return minimap_port_grouped_soa_owned(
                std::forward<p_arguments>(args)...);
		});
}

template <class p_storage, class p_function>
inline void with_checked_minimizer_fragment_reader(
		p_storage& storage,
		p_function&& function) {
	try {
		std::forward<p_function>(function)();
		storage.close_checked();
	} catch (const bioseq::postgres::support::postgres_error&) {
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

struct opportunistic_index_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

struct opportunistic_fragment_index_storage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<opportunistic_index_chunk> chunks{};

    [[noreturn]] void index_error(std::string_view msg) const {
		bioseq::postgres::support::invoke_pg_as_cpp([msg] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq opportunistic index: %.*s",
					static_cast<int>(msg.size()), msg.data())));
		});
		pg_unreachable();
    }

    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header
    validate_minimizer_blob_or_error(
            const char* p,
            std::size_t bytes,
            std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
		using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
		using bucket_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) {
            index_error("minimizer blob truncated header");
        }
        header_type h{};
        std::memcpy(&h, p, sizeof(header_type));
		if (h.magic != bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_magic
			|| h.layout_revision
				!= bioseq::indexing::minimizer_hashmap_fragment_index::
					k_encoded_layout_revision) {
			index_error("minimizer blob magic/layout");
        }
        const std::size_t expect =
            sizeof(header_type) + sizeof(bucket_type) * h.bucket_count + sizeof(std::uint64_t) * h.total_positions;
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
        for (const opportunistic_index_chunk& chunk : chunks) {
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
        for (const opportunistic_index_chunk& chunk : chunks) {
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
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) {
        for (opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return reinterpret_cast<const p_value*>(chunk.bytes.data());
            }
        }
        index_error("chunk missing");
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0
                ? 1
                : (bytes_count + bioseq::indexing::k_max_index_chunk_size_bytes - 1)
                    / bioseq::indexing::k_max_index_chunk_size_bytes;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::k_max_index_chunk_size_bytes);
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

    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(p_value) != 0) {
                index_error("chunk payload is not divisible by the value type size");
            }
            opportunistic_index_chunk chunk{};
            chunk.sub_key = sub_key;
            chunk.chunk_no = static_cast<std::uint32_t>(chunk_no);
            chunk.bytes.assign(payload_bytes, char{0});
            const std::size_t next =
                writer(chunk_no, reinterpret_cast<p_value*>(chunk.bytes.data()),
                       payload_bytes / sizeof(p_value));
            if (next < chunk_no) {
                index_error("writer moved backwards");
            }
            chunks.push_back(std::move(chunk));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(it);
    }

    template <class p_function>
    void read_chunks_in_range(
            bioseq::indexing::index_sub_key_t lo,
            bioseq::indexing::index_sub_key_t hi,
            p_function&& cb) {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key >= lo && chunk.sub_key < hi) {
                cb(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    void for_each_sub_key(auto&& fn) const {
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
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
                [lo, hi](const opportunistic_index_chunk& chunk) {
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
            bioseq::parallelism::memory_permit permit) {
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

// Transient minimizer-sequence storage backing a real no-index per-sequence
// build. Unlike opportunistic_fragment_index_storage (which refuses external-sort
// spill), this delegates spill/merge to a real sorted_pair_run_store so it satisfies
// is_external_sort_spill_storage + is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
// that direct_minimizer_bucket_index<p_sequence> requires. No persisted sidecar rows.
struct opportunistic_sequence_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::size_t chunk_no = 0;
    std::vector<char> bytes;
};

struct opportunistic_sequence_index_storage {
    using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

    std::vector<opportunistic_sequence_chunk> chunks;
    bioseq::storage::indexing::sorted_pair_run_store sorted_runs;
    std::size_t run_buffer_bytes_value = std::size_t{256} << 20;

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return run_buffer_bytes_value;
    }

    void clear_entity() {
        chunks.clear();
        sorted_runs.release_runs();
    }

    // Required by is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
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

    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key != sub_key) {
                continue;
            }
            const std::size_t cap = chunk.bytes.size() / sizeof(p_value);
            writer(
                chunk.chunk_no,
                reinterpret_cast<p_value*>(chunk.bytes.data()),
                cap);
        }
    }

    template <class p_value, class p_function>
    void write_to(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        write_to_each<p_value>(sub_key, std::forward<p_function>(writer));
    }

    [[nodiscard]] const char* read_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        return chunk_ref(sub_key, chunk_no).bytes.data();
    }

    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        return reinterpret_cast<const p_value*>(chunk_ref(sub_key, chunk_no).bytes.data());
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
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++count;
            }
        }
        return count;
    }

    // Reports the distinct sub-keys required by the storage concept.
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t count = 0;
        for_each_sub_key([&count](bioseq::indexing::index_sub_key_t) { ++count; });
        return count;
    }

    template <class p_function>
    void for_each_sub_key(p_function&& fn) const {
        bioseq::indexing::index_sub_key_t last = 0;
        bool have_last = false;
        // chunks are appended in build order; emit each distinct sub_key once.
        std::vector<bioseq::indexing::index_sub_key_t> seen;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
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
        // The transient no-index minimizer-sequence path immediately enters the next target
        // next target range. Keep these runs file-backed so resident pair
        // vectors do not consume the admission budget needed by the nested GPU
        // sketch dispatch.
        return sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
        std::vector<sorted_pair_t> sorted,
        std::uint64_t admitted_bytes,
        bioseq::parallelism::memory_permit permit)
    {
        (void)admitted_bytes;
        const std::size_t run_index = sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
        permit.release();
        return run_index;
    }

    template <class p_function>
    void merge_runs(p_function&& on_kmer) {
        sorted_runs.merge_runs(std::forward<p_function>(on_kmer));
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

    template <class p_function>
    void read_chunks_in_range(
        bioseq::indexing::index_sub_key_t first,
        bioseq::indexing::index_sub_key_t last,
        p_function&& f) const
    {
        for (bioseq::indexing::index_sub_key_t sub_key = first;
             sub_key < last;
             ++sub_key) {
            for (const opportunistic_sequence_chunk& chunk : chunks) {
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
		using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        if (bytes < sizeof(header_type)) {
            index_error("opportunistic minimizer-sequence: truncated blob");
        }
        header_type header{};
        std::memcpy(&header, blob, sizeof(header));
        if (expected_bucket_count != 0u
            && header.bucket_count != expected_bucket_count) {
            index_error("opportunistic minimizer-sequence: unexpected bucket count");
        }
        return header;
    }

    [[noreturn]] void index_error(std::string_view message) const {
		bioseq::postgres::support::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq opportunistic minimizer-sequence index: %.*s",
					static_cast<int>(message.size()), message.data())));
		});
		pg_unreachable();
    }

private:
    opportunistic_sequence_chunk& ensure_chunk(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no)
    {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        chunks.push_back(opportunistic_sequence_chunk{
            .sub_key = sub_key,
            .chunk_no = chunk_no,
            .bytes = {},
        });
        return chunks.back();
    }

    [[nodiscard]] const opportunistic_sequence_chunk& chunk_ref(
        bioseq::indexing::index_sub_key_t sub_key,
        std::size_t chunk_no) const
    {
        for (const opportunistic_sequence_chunk& chunk : chunks) {
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


[[nodiscard]] bioseq::postgres::scheduler::persist_target
make_fragment_index_persist_target(
        const batch_processor::scanner_pg_context& ctx,
        const bioseq::storage::sequence_locator& loc,
        std::uint64_t sequence_rowno,
        std::string_view index_name = minimizers_per_fragment_index_name) {
    return bioseq::postgres::scheduler::persist_target{
        .db_oid = ctx.db_oid,
        .user_relid = ctx.user_relid,
        .tablespace_oid = ctx.tablespace_oid,
        .main_relfilenode = ctx.main_relfilenode,
        .column_attno = ctx.mmsearch_seq_attno,
        .bucket_idx = static_cast<std::uint8_t>(loc.bucket_index),
        .shard_idx = loc.shard_index,
        .sequence_rowno = sequence_rowno,
        .index_name = index_name,
    };
}


[[nodiscard]] bool can_defer_recompute_minimizer_persist(
        const batch_processor::scanner_pg_context& ctx,
        const minimap2_parameters& params) noexcept {
    return ctx.recompute_async_persist_active
        && ctx.index_async_persist_active
        && OidIsValid(ctx.user_relid)
        && params.kmer_size == 15
        && params.window_size == 10
        // Existing asynchronous minimizer-fragment/minimizer-sequence payloads
        // have no minimizer-scheme field.
        // Bounded ntHash recompute remains bounded, but persistence must wait for the
        // scheme-tagged READY-generation writer; never relabel it Exact/scalar.
        && pipe::target_index_sketch_backend_from_params(params)
            != bioseq::minimizers::minimizer_sketch_backend::
                bounded_nthash_simd;
}

// Select the target index sketch backend for recompute from
// the resolved params (Vulkan only when all three high-level gates are on).
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
target_backend_from_params_for_recompute(const minimap2_parameters& params) noexcept
{
    return pipe::target_index_sketch_backend_from_params(params);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
pg_segment_unit_ranges(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        const minimap2_parameters& params)
{
    const auto backend =
        (pg_vulkan_seed_lookup_selected(params)
         || target_backend_from_params_for_recompute(params)
             == bioseq::minimizers::minimizer_sketch_backend::vulkan_segment)
            ? bioseq::minimizers::minimizer_sketch_backend::vulkan_segment
            : bioseq::minimizers::minimizer_sketch_backend::scalar;
    return pipe::segment_unit_ranges_for_backend(
        total_bp,
        query_len,
        lpu,
        backend);
}

// Build a real transient minimizers_per_sequence index
// (GPU-backed when Vulkan is selected), assemble it for the query, and run the
// indexed aligner. Shared by BOTH the fast no_index recompute path and the
// deferred-persist recompute path, so neither serves a no_index x per_sequence
// cell as a minimizer-fragment-shaped segmented relabel. Streams from the lazy PostgreSQL
// target via the GPU sorted-pair seam (device emit plus host pair sort) with no
// whole-target resident materialization, so
// 2/4 Gbp targets do not OOM.
template <sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection build_and_align_transient_minimizer_sequence(
    bioseq::storage::lazy_sequence_view<p_alphabet> lazy_query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    const minimap2_parameters& params,
    sequence_size_t total_bp_size,
    std::size_t target_letters_per_unit)
{
    match_result_collection collection;
	// Every PostgreSQL terminal error below crosses a true
	// invoke_pg_as_cpp leaf. Keep this orchestration in ordinary C++ so minimizer-sequence
	// chunks, sorted runs, GPU cache, query stats, assembled indexes, and
	// segment results unwind normally on postgres_error.
	opportunistic_sequence_index_storage minimizer_sequence_store;
		minimizer_sequence_store.run_buffer_bytes_value =
            bioseq::settings::index_build::run_buffer_bytes > 0
                ? static_cast<std::size_t>(
                    bioseq::settings::index_build::run_buffer_bytes)
                : (std::size_t{256} << 20);

        // direct_minimizer_bucket_index::build is pinned to w=10,k=15. The
        // transient read side must use the same minimizer parameters even if
        // apply_size_heuristic() reduced the caller's k/w for short reads.
        // Keep the caller's scoring/seed-count adjustments.
		minimap2_parameters minimizer_sequence_params = params;
		minimizer_sequence_params.kmer_size = 15;
		minimizer_sequence_params.window_size = 10;
		if (minimizer_sequence_params.highway_kernels == nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "transient minimizer-sequence build has no selected Highway minimizer table");
        }

		const auto backend =
			target_backend_from_params_for_recompute(minimizer_sequence_params);
		const bool scalar_sketch_selected =
			backend == bioseq::simd::minimizer_sketch_backend::scalar;
		const bool exact_simd_sketch_selected =
			backend == bioseq::simd::minimizer_sketch_backend::exact_simd;
		bioseq::indexing::direct_minimizer_bucket_build_stats
			scalar_target_build_stats{};
        bioseq::indexing::direct_minimizer_bucket_index<
            opportunistic_sequence_index_storage>::build(
				minimizer_sequence_store,
                target_view,
                backend,
				&minimizer_sequence_params.highway_kernels->minimizers,
				scalar_sketch_selected
					? &scalar_target_build_stats : nullptr,
				minimizer_sequence_params.alphabet_code);

        auto query_sketch =
            sketch_query_for_index_selection<minimap_sketch_data>(
                lazy_query_view,
				minimizer_sequence_params,
				static_cast<std::size_t>(minimizer_sequence_params.window_size),
				static_cast<std::size_t>(minimizer_sequence_params.kmer_size));
        std::optional<bioseq::indexing::gpu_minimizer_index_cache> gpu_index_cache;
		if (pg_vulkan_seed_lookup_selected(minimizer_sequence_params)) {
            gpu_index_cache.emplace(pg_gpu_index_resident_limit_bytes());
        }
        auto* gpu_index_cache_ptr =
            gpu_index_cache ? &*gpu_index_cache : nullptr;
        const std::uint64_t transient_identity_base =
            static_cast<std::uint64_t>(
				reinterpret_cast<std::uintptr_t>(&minimizer_sequence_store));
        bioseq::indexing::minimizer_assembly_scratch scratch;
		auto run_minimizer_sequence_index =
			[&minimizer_sequence_store, lazy_query_view, target_view, &query_sketch,
			 &minimizer_sequence_params,
             total_bp_size, &scratch, gpu_index_cache_ptr,
             transient_identity_base, backend](
                    sequence_size_t clip_lo_bp,
                    sequence_size_t clip_hi_bp) {
            auto index =
                bioseq::indexing::direct_minimizer_bucket_index<
                    opportunistic_sequence_index_storage>::assemble_for_query_segment(
						minimizer_sequence_store,
                        query_sketch.as_span(),
                        total_bp_size,
						minimizer_sequence_params.max_occ,
                        clip_lo_bp,
                        clip_hi_bp,
                        scratch);
            const auto gpu_identity =
                pg_gpu_index_identity(
                    transient_identity_base,
                    minimizers_per_sequence_index_name,
                    clip_lo_bp,
                    clip_hi_bp,
                    backend);
            return minimap_port_fragment_index_vector_pg_checked(
                lazy_query_view,
                target_view,
                query_sketch.minimizers,
                index,
				minimizer_sequence_params,
                gpu_index_cache_ptr,
                gpu_identity);
        };
        if (total_bp_size <= postgres_segmentation_threshold_bp) {
			collection = run_minimizer_sequence_index(0, total_bp_size);
        } else {
            const std::size_t lpu = target_letters_per_unit;
            match_result_collection merged_collection;
            std::vector<match_result>& merged = merged_collection.results;
            const sequence_size_t query_len = lazy_query_view.bp_size();
            for (auto sr : pg_segment_unit_ranges(
					 total_bp_size, query_len, lpu, minimizer_sequence_params)) {
                const sequence_size_t clip_lo_bp =
                    static_cast<sequence_size_t>(sr.cs_u) * lpu;
                const sequence_size_t clip_hi_bp =
                    std::min(
                        static_cast<sequence_size_t>(sr.ce_u) * lpu,
                        total_bp_size);
                match_result_collection seg_res =
					run_minimizer_sequence_index(clip_lo_bp, clip_hi_bp);
                merged_collection.combine_execution_stats(seg_res);
                for (auto& r : seg_res.results) {
                    if (r.target_start >= sr.core_lo_bp
                        && r.target_start < sr.core_hi_bp) {
                        merged.push_back(std::move(r));
                    }
                }
            }
            collection = std::move(merged_collection);
        }
		if (exact_simd_sketch_selected) {
			if (!query_sketch.exact_simd_execution_stats_available
				|| !query_sketch.exact_simd_execution_stats.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<logging_level::error>(
                    "Persisted-index two_stack_nthash query lacks actual execution stats");
                std::abort();
            }
			collection.record_exact_simd_query_execution_stats(
				query_sketch.exact_simd_execution_stats);
		} else if (scalar_sketch_selected) {
			if (!query_sketch.scalar_execution_stats_available
				|| !query_sketch.scalar_execution_stats.is_coherent()
				|| query_sketch.exact_simd_execution_stats_available
				|| query_sketch.bounded_nthash_execution_stats_available
				|| !scalar_target_build_stats.is_internally_consistent()
                || backend
                    != bioseq::simd::
                        minimizer_sketch_backend::scalar) {
                bioseq_log<logging_level::error>(
					"Transient minimizer-sequence scalar build/query lacks actual execution stats");
                std::abort();
            }
			collection.record_scalar_query_minimizer_execution_stats(
				query_sketch.scalar_execution_stats);
			const scalar_minimizer_execution_stats target_stats{
				.calls = scalar_target_build_stats.scalar_collection_calls,
				.input_symbols = scalar_target_build_stats.input_symbols,
				.saturated = scalar_target_build_stats.saturated,
			};
			if (!target_stats.is_coherent()) {
                bioseq_log<logging_level::error>(
					"Transient minimizer-sequence scalar target stats conversion was rejected");
                std::abort();
            }
			collection.record_scalar_target_minimizer_execution_stats(
				target_stats);
		} else if (query_sketch.exact_simd_execution_stats_available) {
            bioseq_log<logging_level::error>(
                "Non-two_stack_nthash persisted-index query emitted two_stack_nthash stats");
            std::abort();
        }
    return collection;
}

[[nodiscard]] bool stage_recompute_minimizer_sequence_build_if_selected(
        const batch_processor::scanner_pg_context& ctx,
        const bioseq::storage::sequence_locator& loc,
        std::uint64_t sequence_rowno,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        const minimap2_parameters& params,
        std::string_view reason) {
    if (!can_defer_recompute_minimizer_persist(ctx, params)) {
        if (ctx.recompute_async_persist_active
            && target_backend_from_params_for_recompute(params)
                == bioseq::minimizers::minimizer_sketch_backend::
                    bounded_nthash_simd) {
            bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} reason={} because persistence requires a scheme-tagged READY generation; recompute continues without fallback",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.logical_length),
                    reason);
        } else if (ctx.recompute_async_persist_active
            && (params.kmer_size != 15 || params.window_size != 10)) {
            bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} reason={} because active params are k={} w={} but persisted minimizer indexes are pinned to k=15 w=10",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.logical_length),
                    reason,
                    params.kmer_size,
                    params.window_size);
        } else if (!ctx.recompute_async_persist_active
                   || !ctx.index_async_persist_active
                   || !OidIsValid(ctx.user_relid)) {
            bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} reason={} gates recompute_async={} index_async={} user_relid_valid={}",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(loc.logical_length),
                    reason,
                    ctx.recompute_async_persist_active,
                    ctx.index_async_persist_active,
                    OidIsValid(ctx.user_relid));
        }
        return false;
    }

    const auto choice = choose_deferred_minimizer_persist_index(ctx);
    if (!choice.has_value()) {
        bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} reason={} because no minimizer index is enabled for UPDATE_ON_INSERT",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason);
        return false;
    }
    if (choice->kind != deferred_minimizer_index_kind::per_sequence) {
        bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build skipped row={} index={} target_bp={} reason={} because deferred choice is not per-sequence",
                static_cast<unsigned long long>(sequence_rowno),
                choice->index_name,
                static_cast<unsigned long long>(loc.logical_length),
                reason);
        return false;
    }

    const auto persist_target =
        make_fragment_index_persist_target(
            ctx, loc, sequence_rowno, choice->index_name);
    const bool staged = bioseq::postgres::scheduler::stage_async_minimizer_sequence_build(
        persist_target,
        loc,
        alphabet_code,
        storage_unit_byte_width,
        last_fragment_padding,
        static_cast<std::int32_t>(
            bioseq::settings::index::sequence_minimizer_sort::algorithm.get_value()),
        static_cast<int>(
            bioseq::scheduler::settings::pipeline::mode.get_value()),
        bioseq::scheduler::settings::backend::max_concurrency,
        bioseq::settings::index::memory_cap_mb,
        static_cast<std::int32_t>(  // Thread the selected backend into the async worker.
            target_backend_from_params_for_recompute(params)),
        params.highway_executed_target);
    bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build row={} index={} target_bp={} reason={} async_staged={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice->index_name,
            static_cast<unsigned long long>(loc.logical_length),
            reason,
            staged);
    return staged;
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection
run_recompute_with_deferred_minimizer_persist(
        bioseq::storage::lazy_sequence_view<p_alphabet> lazy_query,
        bioseq::storage::lazy_sequence_view<p_alphabet> resident_target,
        const minimap2_parameters& params,
        const batch_processor::scanner_pg_context& ctx,
        const bioseq::storage::sequence_locator& loc,
        std::uint64_t sequence_rowno,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        const deferred_minimizer_persist_choice& choice) {
    if (choice.kind == deferred_minimizer_index_kind::per_sequence) {
        // Real minimizer-sequence no-index route: build a transient minimizers_per_sequence
        // index (GPU-backed when Vulkan is selected), assemble it for the query, and run
        // the indexed aligner. No minimizer-fragment relabelling. ctx is unused on this transient path.
        (void)ctx;
        (void)alphabet_code;
        (void)storage_unit_byte_width;
        (void)last_fragment_padding;
        match_result_collection collection =
            build_and_align_transient_minimizer_sequence(
            lazy_query,
            resident_target,
            params,
            static_cast<sequence_size_t>(loc.logical_length),
            storage_unit_byte_width * CHAR_BIT
                / bioseq::storage::lazy_sequence_traits<p_alphabet>::
                    k_logical_bits_per_symbol);

        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute real-minimizer-sequence-noindex row={} index={} "
            "target_bp={} persisted=false backend={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.logical_length),
            bioseq::minimizers::minimizer_sketch_backend_name(
                target_backend_from_params_for_recompute(params)));
        return collection;
    }

	(void)ctx;
	(void)alphabet_code;
	(void)storage_unit_byte_width;
	(void)last_fragment_padding;
	match_result_collection collection = minimap_port_segmented_pg_checked(
		lazy_query, resident_target, params);
	bioseq_log<logging_level::debug>("[bioseq] index: recompute external-file persist prohibited row={} index={} target_bp={} constructor_entries={} writer_entries={} async_staged=false",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.logical_length),
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

template <sequence_alphabet p_alphabet>
struct resident_target_for_workers {
    bioseq::storage::admitted_packed_sequence<p_alphabet> owner;
};

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<resident_target_for_workers<p_alphabet>>
try_materialize_target_for_segment_workers(
        bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
        std::uint8_t requested_width,
        std::uint64_t sequence_rowno) {
    const auto plan = bioseq::storage::plan_sequence_access(
        target_view,
        bioseq::storage::sequence_access_requirement::bounded_random_window,
        0u,
        target_view.bp_size(),
        requested_width);
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(plan.admission_bytes);
    if (!permit) {
        bioseq_log<logging_level::debug>(
            "[bioseq] resident target: row={} target_bp={} admitted_bytes={} "
            "not admitted; using scanner_pg serial fallback",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(target_view.bp_size()),
            static_cast<unsigned long long>(plan.admission_bytes));
        return std::nullopt;
    }

    const std::uint64_t materialize_started_ns = dispatch_host_now_ns();
    auto owner = bioseq::storage::materialize_packed_sequence(
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
        * descriptor.storage_unit_width;

    bioseq_log<logging_level::debug>(
        "[bioseq] resident target: row={} target_bp={} packed_bytes={} "
        "materialize_ns={} materialized for segment workers",
        static_cast<unsigned long long>(sequence_rowno),
        static_cast<unsigned long long>(target_view.bp_size()),
        static_cast<unsigned long long>(packed_bytes),
        static_cast<unsigned long long>(materialize_elapsed_ns));

    return resident_target_for_workers<p_alphabet>{std::move(owner)};
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Compare every locator field before one resident target is shared by a
/// query-parallel batch. The comparison stays PostgreSQL-free so it can also
/// serve as a preflight stats before any worker is entered.
[[nodiscard]] constexpr bool same_dispatch_target_locator(
        const bioseq::storage::sequence_locator& left,
        const bioseq::storage::sequence_locator& right) noexcept {
    return left.payload_version == right.payload_version
        && left.bucket_index == right.bucket_index
        && left.reserved_16 == right.reserved_16
        && left.shard_index == right.shard_index
        && left.fragment_count == right.fragment_count
        && left.reserved_32 == right.reserved_32
        && left.logical_length == right.logical_length
        && left.first_fragment_row == right.first_fragment_row
        && left.source_epoch == right.source_epoch;
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Validate the complete immutable hash-major reader identity before it is
/// borrowed concurrently. No worker may observe a partially authenticated,
/// mixed-layout or storage-profile-transcoded prepared group.
[[nodiscard]] inline bool hash_major_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_hash_major_reader.has_value()
		|| group.persisted_fragment_mapping_guard.has_value()
        || group.persisted_fragment_reader.has_value()
		|| group.persisted_grouped_soa_reader.has_value()
        || group.target_index
        || group.target_index_source
            != ::bioseq::pipeline_stats::target_index_source::persisted
        || group.persisted_generation_id == 0u
        || group.persisted_source_identity == 0u
        || !group.persisted_hash_major_source_identity_present
        || group.persisted_hash_major_source_identity
            == std::array<std::byte, 16>{}
        || group.persisted_source_identity
            != hash_major_source_diagnostic_token(
                group.persisted_hash_major_source_identity)
        || !group.query_hash_union.empty()
        || group.query_hash_union.capacity() != 0u
        || group.persisted_hash_major_reader->target_length() != target_bp_size) {
        return false;
    }

    const auto& receipt = group.persisted_read_stats.hash_major;
    const auto& memory =
        group.persisted_hash_major_reader->memory_stats();
    const std::uint64_t stage_peak = std::max({
        receipt.directory_stage_peak_bytes,
        receipt.control_stage_peak_bytes,
        receipt.posting_stage_peak_bytes,
    });
    return receipt.layout_revision
            == bioseq::indexing::k_hash_major_minimizer_layout_revision
        && receipt.sessions == 1u
        && receipt.generation_id == group.persisted_generation_id
        && receipt.source_identity
            == group.persisted_hash_major_source_identity
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
        && receipt.peak_admitted_bytes == stage_peak
        && receipt.retained_owner_bytes == receipt.admitted_bytes
        && receipt.admitted_bytes
            == group.persisted_hash_major_reader->admitted_bytes()
        && receipt.generic_source_fallback_calls == 0u
        && receipt.failures == 0u
        && !receipt.saturated;
}

[[bioseq::template_flatten_when("shape", "root")]]
/// grouped-SoA counterpart of the concurrent-reader stats. The reader owns only
/// independent fragment address plans. Query routing/count reduction is
/// transient wave state, and every persisted posting remains owned by exactly
/// one fragment.
[[nodiscard]] inline bool fragment_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_fragment_reader.has_value()
		|| !group.persisted_fragment_mapping_guard.has_value()
		|| !*group.persisted_fragment_mapping_guard
		|| group.persisted_grouped_soa_reader.has_value()
        || group.persisted_hash_major_reader.has_value()
        || group.target_index
        || group.target_index_source
            != ::bioseq::pipeline_stats::target_index_source::persisted
        || group.persisted_generation_id == 0u
        || group.persisted_source_identity == 0u
        || group.persisted_hash_major_source_identity_present
        || group.persisted_hash_major_source_identity
            != std::array<std::byte, 16>{}
        || !group.query_hash_union.empty()
        || group.query_hash_union.capacity() != 0u
        || group.persisted_fragment_reader->target_length()
            != target_bp_size) {
        return false;
    }
    const auto& read = group.persisted_read_stats;
    const auto& reader = *group.persisted_fragment_reader;
    const auto memory =
        reader.memory_stats();
    const bool exact_lookup_backing_identity = [&]() noexcept {
        switch (group.persisted_fragment_lookup_route) {
        case bioseq::indexing::minimizer_fragment_lookup_route::
                direct_compact_hash_lookup:
            return reader.compact_direct_ready()
                && reader.compact_direct_only()
                && reader.mapped_grouped_soa_payload_bytes() == 0u
                && reader.grouped_soa_directory_entry_count() == 0u
                && memory.directory_capacity_bytes == 0u
                && memory.ordinal_route_capacity_bytes == 0u
                && read.directory_chunks_read == 0u
                && read.directory_bytes_read == 0u;
        case bioseq::indexing::minimizer_fragment_lookup_route::
                grouped_soa_control_scans:
            return !reader.compact_direct_ready()
                && !reader.compact_direct_only()
                && reader.mapped_grouped_soa_payload_bytes() > 0u
                && reader.grouped_soa_directory_entry_count() > 0u
                && memory.directory_capacity_bytes > 0u
                && memory.ordinal_route_capacity_bytes > 0u
                && read.directory_chunks_read > 0u
                && read.directory_bytes_read > 0u;
        }
		return false;
	}();
	const bool deferred_query_set_identity =
		group.compact_direct_query_set_validation_pending
		? group.persisted_fragment_lookup_route
				== bioseq::indexing::minimizer_fragment_lookup_route::
					direct_compact_hash_lookup
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

[[bioseq::template_flatten_when("shape", "root")]]
/// Attach the retained query-sketch stats after a worker has joined. The
/// prebuilt-minimizer entry point must not publish its own query stats; this
/// check makes duplicate attachment fail closed on the PostgreSQL thread.
inline void attach_retained_hash_major_query_stats(
        match_result_collection& collection,
        const pipe::minimizer_range_execution_stats& stats) {
    const bool exact_query_stats_absent =
        !collection.exact_simd_query_execution_stats_available
        && collection.exact_simd_query_execution_stats
            == decltype(collection.exact_simd_query_execution_stats){};
    const bool scalar_query_stats_absent =
        collection.scalar_pipeline_stats.query_minimizers
            == decltype(collection.scalar_pipeline_stats
                .query_minimizers){};
    const bool bounded_query_stats_absent =
        !collection.bounded_nthash_query_execution_stats_available
        && collection.bounded_nthash_query_execution_stats
            == decltype(collection.bounded_nthash_query_execution_stats){};
    if (!scalar_query_stats_absent
        || !exact_query_stats_absent
        || !bounded_query_stats_absent) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "persisted query sketch stats were recorded more than once");
    }
    if (stats.exact_simd_available) {
        collection.record_exact_simd_query_execution_stats(
            stats.exact_simd);
    }
    if (stats.bounded_nthash_available) {
        collection.record_bounded_nthash_query_execution_stats(
            stats.bounded_nthash);
    }
    if (stats.scalar_available) {
        collection.record_scalar_query_minimizer_execution_stats(
            stats.scalar);
    }
}

/**
 * Promote one decoded sequence locator through the statement's exact source
 * provider, then expose only the canonical lazy facade to the algorithm.
 * The provider remains scan-owned; the read session owns the facade adapter
 * and is closed only after the callback has joined/drained all algorithm work.
 */
template <class p_alphabet, class p_function>
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
inline void with_source_provider_pg_locator(
        batch_processor::scanner_pg_context& context,
        const bioseq::storage::sequence_locator& locator,
        const AttrNumber column_attno,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        const std::uint16_t last_padding_letters,
        pg_sequence_read_session* active_read_session,
        p_function&& callback) {
    auto* provider = context.source_provider;
    auto* registry = provider != nullptr
        ? provider->lease.registry : nullptr;
    const bool exact_provider = provider != nullptr
        && provider->open
        && provider->kind
            == bioseq::storage::pg_sequence_source_provider_kind::scan
        && provider->lease.owned
        && registry != nullptr
        && provider->lease.provider_generation == registry->generation
        && registry->accepting_lookups
        && !registry->provider_closed
        && !registry->lease_fault
        && !registry->xact_ended
        && registry->backend_thread.is_current()
        && registry->user_relid == context.user_relid
        && registry->database_oid == context.db_oid
        && registry->tablespace_oid == context.tablespace_oid
        && registry->main_relfilenode == context.main_relfilenode
        && registry->authoritative_snapshot == context.snapshot
        && context.snapshot != nullptr
        && IsMVCCSnapshot(context.snapshot)
        && context.snapshot->regd_count > 0
        && column_attno > 0
        && context.parent_mctx != nullptr;
    if (!exact_provider) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "locator sequence has no exact statement source-provider route");
    }

    bioseq::pg_types::seq* locator_stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [&context, &locator, alphabet_code, storage_unit_byte_width,
         last_padding_letters] {
            return bioseq::storage::alloc_locator_stub_in(
                context.parent_mctx,
                alphabet_code,
                storage_unit_byte_width,
                last_padding_letters,
                locator.logical_length,
                locator);
        });
    const Datum promoted = bioseq::storage::promote_pg_sequence_locator(
        provider,
        column_attno,
        PointerGetDatum(locator_stub),
        context.parent_mctx);
    const pg_sequence_descriptor descriptor =
        bioseq::postgres::support::invoke_pg_as_cpp([promoted] {
            return ::classify_and_describe_sequence(promoted);
        });
    if (!descriptor.alphabet_bound
        || (descriptor.kind
                != pg_sequence_datum_kind::expanded_read_only
            && descriptor.kind
                != pg_sequence_datum_kind::expanded_read_write)
        || descriptor.alphabet_code != alphabet_code
        || descriptor.storage_unit_byte_width != storage_unit_byte_width
        || descriptor.bp_length != locator.logical_length) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "source-provider promotion returned mismatched sequence metadata");
    }

    if (active_read_session != nullptr) {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_read_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *active_read_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), active_read_session);
        return;
    }

    auto* local_session = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp([local_session] {
            ::finish_pg_sequence_read_session(local_session, true);
        });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

template <class p_alphabet, class p_function>
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
inline void with_pg_sequence_recipe(
        batch_processor::scanner_pg_context& context,
        const Datum recipe,
        const pg_sequence_descriptor& descriptor,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        p_function&& callback) {
    const bool expanded = recipe != Datum{0}
        && VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(recipe))
        && (descriptor.kind == pg_sequence_datum_kind::expanded_read_only
            || descriptor.kind
                == pg_sequence_datum_kind::expanded_read_write);
    if (!expanded || !descriptor.alphabet_bound
        || descriptor.alphabet_code != alphabet_code
        || descriptor.storage_unit_byte_width != storage_unit_byte_width
        || context.parent_mctx == nullptr) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "retained sequence recipe metadata is invalid");
    }

    if (context.read_session != nullptr) {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [read_session = context.read_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *read_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), context.read_session);
        return;
    }

    auto* local_session = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp([local_session] {
            ::finish_pg_sequence_read_session(local_session, true);
        });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

template <class p_sequence, class p_function>
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
inline void with_source_provider_pg_target(
        batch_processor::scanner_pg_context& context,
        const bioseq::storage::sequence_locator& locator,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        const std::uint16_t last_padding_letters,
        pg_sequence_read_session* active_read_session,
        p_function&& callback) {
    using alphabet =
        typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    with_source_provider_pg_locator<alphabet>(
        context,
        locator,
        static_cast<AttrNumber>(context.mmsearch_seq_attno),
        alphabet_code,
        storage_unit_byte_width,
        last_padding_letters,
        active_read_session,
        std::forward<p_function>(callback));
}

// Keep the PG-backed scanner row path as a separate function so GCC does not
// compile the whole typed scheduler as one very large control-flow graph.
template <class p_sequence>
[[bioseq::template_flatten_when("shape", "scanner_pg")]]
[[nodiscard]] match_result_collection process_scanner_pg_row_typed(
    bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type>>
        target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    const minimap2_parameters& dispatch_params,
    const minimap2_parameters& params,
    const bool per_row_query_mode,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>&
        cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session)
{
    match_result_collection collection;
    // Recompute-owned and per-row sketches are independent operations. A
    // broadcast persisted-query sketch is shared across rows: every consuming
    // job receives its stats for route acceptance, while the caller accounts
    // the shared operation only once on the first stats-valid consumer.
    consumed_shared_cached_query_stats = false;
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
				"bioseq batch join CPU prepared-target identity is invalid");
		}
	}
    scanner_pg_ctx->target_index_sources[i] =
        ::bioseq::pipeline_stats::target_index_source::recomputed;
    std::optional<std::vector<minimap_sketch_data>> row_query_minimizers;
    std::optional<pipe::minimizer_range_execution_stats>
        row_query_minimizer_stats;
    const batch_processor::cpu_prepared_query_sketch*
        retained_prepared_query_sketch = nullptr;
    const sequence_size_t query_bp_size = query_view.bp_size();
    const auto unobserved_query_view = query_view;
    const auto lazy_query_view =
        unobserved_query_view.with_payload_allocation_observer(
            std::addressof(
                scanner_pg_ctx->mmsearch_query_payload_observer));
    const auto fq_pg_view = lazy_query_view;
    const std::size_t target_letters_per_unit =
        bioseq::storage::packed_letters_per_unit(
            storage_unit_byte_width,
            static_cast<std::uint8_t>(p_sequence::letter_bit_width));
    if (target_letters_per_unit == 0u) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "PG lazy facade has invalid packed storage geometry");
    }
    const auto& loc_opt = scanner_pg_ctx->target_locators[i];
    const bool persisted_required =
        scanner_pg_ctx->index_read_requirement
        == bioseq::indexing::index_read_requirement::
            require_compatible_persisted;
    if (persisted_required
		&& (cpu_prepared_target == nullptr
			|| selected_vulkan_execution(params))) {
		raise_required_persisted_index_error(
			scanner_pg_ctx,
			scanner_pg_ctx->target_rownos[i],
			"the live persisted reader is available only through the retained "
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
    if (target_recipe_view.has_value()) {
        if (loc_opt.has_value() || target_hdr != nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "target recipe is not exclusive with flat/locator storage");
        }
        if (persisted_required) {
            raise_required_persisted_index_error(
                scanner_pg_ctx,
                scanner_pg_ctx->target_rownos[i],
                "a transformed target has no compatible persisted base-row index");
        }
        const auto observed_target = target_recipe_view->
            with_payload_allocation_observer(std::addressof(
                scanner_pg_ctx->mmsearch_target_payload_observer));
        collection = target_bp_size > postgres_segmentation_threshold_bp
            ? minimap_port_segmented_pg_checked(
                lazy_query_view, observed_target, params)
            : minimap_port_raw_pg_checked(
                lazy_query_view, observed_target, params);
    } else if (loc_opt.has_value()) {
        // Compute last-fragment padding from the canonical facade
        // geometry. For
        // total_bp = N, padding = ceil(N/lpu)*lpu - N.
        const std::uint64_t _remainder =
            loc_opt->logical_length % target_letters_per_unit;
        const std::size_t _padding = static_cast<std::size_t>(
            _remainder == 0u
                ? 0u : target_letters_per_unit - _remainder);
        with_source_provider_pg_target<p_sequence>(
            *scanner_pg_ctx,
            *loc_opt,
            alphabet_code,
            storage_unit_byte_width,
            static_cast<std::uint16_t>(_padding),
            active_read_session,
            [scanner_pg_ctx, cpu_prepared_target, &dispatch_params, &params,
             min_bp, per_row_query_mode, persisted_required,
             &row_query_minimizers,
             &cached_query_minimizers, &row_query_minimizer_stats,
             &cached_query_minimizer_stats, query_bp_size, lazy_query_view,
             fq_pg_view, target_bp_size, alphabet_code,
             storage_unit_byte_width, i, &loc_opt, _padding, &pool,
             target_letters_per_unit,
             &collection, &row_consumed_persisted_query_sketch,
             &retained_prepared_query_sketch,
             &query_sketch_host_wall_ns](
                    auto ft_pg_view, pg_sequence_read_session*) {
        ft_pg_view = ft_pg_view.with_payload_allocation_observer(
            std::addressof(
                scanner_pg_ctx->mmsearch_target_payload_observer));
        // Assemble from the stored
        // per-fragment minimizer index if present
        // (deserialise + globalise, no re-collection);
        // otherwise fall back to recompute.
        bool assembled = false;
        /*
         * Persisted-payload reads and opportunistic recompute persistence are
         * independent policies.  In particular, require_absent deliberately
         * clears read_index_active, but index.recompute_async_persistence.enabled must still be
         * able to build and enqueue the default minimizer-fragment index from the source
         * sequence. Keep the minimizer-fragment RAM-first path reachable when both
         * async write gates are captured for this scan; every prepared,
         * sidecar, and assembled-cache read below remains guarded by
         * read_index_active.
         */
        const auto requested_target_backend =
            target_backend_from_params_for_recompute(dispatch_params);
        const bool stored_minimizer_scheme_compatible =
            requested_target_backend
            != bioseq::minimizers::minimizer_sketch_backend::
                bounded_nthash_simd;
        if (scanner_pg_ctx->read_index_active
            && !stored_minimizer_scheme_compatible) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: persisted read skipped for inapplicable storage profile row={} "
                "backend=bounded_nthash_simd reason=payload_has_no_scheme_identity",
                static_cast<unsigned long long>(
                    scanner_pg_ctx->target_rownos[i]));
        }
        const bool opportunistic_minimizer_fragment_recompute_requested =
            !scanner_pg_ctx->read_index_active
            && scanner_pg_ctx->recompute_async_persist_active
            && scanner_pg_ctx->index_async_persist_active
            && stored_minimizer_scheme_compatible
            && scanner_pg_ctx->active_index
                == minimizers_per_fragment_index_name;
        if ((persisted_required
			|| (scanner_pg_ctx->read_index_active
                    && stored_minimizer_scheme_compatible)
                || opportunistic_minimizer_fragment_recompute_requested)
            && OidIsValid(scanner_pg_ctx->user_relid)) {
			using index_storage_type = bioseq::storage::indexing::fragment_index_storage;
            const bool want_seq_recompute =
                scanner_pg_ctx->active_index == minimizers_per_sequence_index_name;
            const bool want_seq = want_seq_recompute;
            // The stored index is ALWAYS w=10,k=15 (pinned at build); the query
            // minimizers + the seed pipeline MUST use the same w,k (NOT the size
            // heuristic's reduced k), exactly as the prebuilt storage-profile branch does.
            minimap2_parameters idx_params = dispatch_params;
            idx_params.kmer_size = 15; idx_params.window_size = 10;
            if (min_bp < 50) { idx_params.min_chain_score = 10; idx_params.min_seed_count = 1; }
            else if (min_bp < 200) { idx_params.min_chain_score = 20; idx_params.min_seed_count = 2; }
            std::optional<bioseq::indexing::gpu_minimizer_index_cache>
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
            auto& query_minimizer_stats =
                per_row_query_mode
                    ? row_query_minimizer_stats
                    : cached_query_minimizer_stats;
            auto ensure_q =
                [&query_minimizers, &query_minimizer_stats,
                 query_bp_size, lazy_query_view, &idx_params,
                 &row_consumed_persisted_query_sketch,
                 &query_sketch_host_wall_ns] {
                if (!query_minimizers.has_value()) {
                    if (!is_minimizer_query_length_representable<
                            minimap_sketch_data>(
                                static_cast<sequence_size_t>(
                                    query_bp_size))) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                            "query exceeds the 28-bit minimizer position domain");
                    }
                    const std::uint64_t query_sketch_started_ns =
                        dispatch_host_now_ns();
                    if (selected_vulkan_execution(idx_params)) {
                        bioseq::gpu::vulkan::vulkan_runtime_error err{
                            "persisted Vulkan query sketch cannot use the CPU prepared-index path"};
                        raise_vulkan_pg_error(
                            "persisted query minimizer sketch", err.what());
                    }
                    sketch_collector qc;
                    const minimizer_sketch_backend backend =
                        pipe::target_index_sketch_backend_from_params(
                            idx_params);
                    auto stats = pipe::minimizer_dispatch_range<
                        minimap_sketch_data>(
                        qc,
                        lazy_query_view,
                        0,
                        query_bp_size,
                        static_cast<size_t>(idx_params.window_size),
                        static_cast<size_t>(idx_params.kmer_size),
                        backend,
                        pipe::exact_simd_range_route::require_continuation,
                        idx_params.highway_kernels);
					if (!pipe::minimizer_range_stats_matches_backend(
							backend,
							stats,
							/*require_nonempty_vector_work=*/false,
							pipe::exact_simd_range_route::require_continuation)) {
						raise_typed_dispatch_pg_error_as_cpp(
							ERRCODE_INTERNAL_ERROR,
							"persisted query sketch stats do not match the selected "
							"minimizer backend");
					}
                    query_minimizers.emplace(std::move(qc).extract_sketches_data());
                    query_minimizer_stats.emplace(std::move(stats));
					query_sketch_host_wall_ns = elapsed_dispatch_host_ns(
						query_sketch_started_ns);
				} else if (!query_minimizer_stats.has_value()) {
					raise_typed_dispatch_pg_error_as_cpp(
						ERRCODE_DATA_CORRUPTED,
						"cached persisted query minimizers have no operation stats");
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
					"bioseq batch join prepared query group is missing");
			}
			if (persisted_required) {
				const bool has_fragment_reader = prepared_group != nullptr
					&& prepared_group->persisted_fragment_reader.has_value();
				const bool has_fragment_mapping_guard =
					prepared_group != nullptr
					&& prepared_group->persisted_fragment_mapping_guard.has_value()
					&& static_cast<bool>(
						*prepared_group->persisted_fragment_mapping_guard);
				const bool has_grouped_soa_reader = prepared_group != nullptr
					&& prepared_group->
						persisted_grouped_soa_reader.has_value();
				const bool has_hash_major_reader = prepared_group != nullptr
					&& prepared_group->persisted_hash_major_reader.has_value();
				const bool has_full_hash_major_source_identity =
					prepared_group != nullptr
					&& prepared_group->
						persisted_hash_major_source_identity_present;
				const bool hash_major_identity_valid =
					prepared_group == nullptr
					|| (!has_hash_major_reader
						? !has_full_hash_major_source_identity
							&& prepared_group->
								persisted_hash_major_source_identity
									== std::array<std::byte, 16>{}
						: has_full_hash_major_source_identity
							&& prepared_group->persisted_source_identity
								== hash_major_source_diagnostic_token(
									prepared_group->
										persisted_hash_major_source_identity));
				const bool canonical_owner_exclusive =
					prepared_group == nullptr
					|| prepared_group->query_hash_union.capacity() == 0u;
				bool hash_major_read_receipt_valid = !has_hash_major_reader;
				if (has_hash_major_reader) {
					const auto& receipt =
						prepared_group->persisted_read_stats.hash_major;
					const auto& memory =
						prepared_group->persisted_hash_major_reader->
							memory_stats();
					const std::uint64_t stage_peak = std::max({
						receipt.directory_stage_peak_bytes,
						receipt.control_stage_peak_bytes,
						receipt.posting_stage_peak_bytes,
					});
					hash_major_read_receipt_valid =
						receipt.layout_revision
							== bioseq::indexing::k_hash_major_minimizer_layout_revision
						&& receipt.sessions == 1u
						&& receipt.generation_id
							== prepared_group->persisted_generation_id
						&& receipt.source_identity
							== prepared_group->
								persisted_hash_major_source_identity
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
						&& receipt.peak_admitted_bytes == stage_peak
						&& receipt.retained_owner_bytes
							== receipt.admitted_bytes
						&& receipt.admitted_bytes
							== prepared_group->
								persisted_hash_major_reader->
									admitted_bytes()
						&& receipt.generic_source_fallback_calls == 0u
						&& receipt.failures == 0u
						&& !receipt.saturated;
				}
				if (prepared_group == nullptr
					|| !batch_processor::exactly_one_native_persisted_reader(
							has_fragment_reader,
							has_grouped_soa_reader,
							has_hash_major_reader)
					|| has_fragment_reader != has_fragment_mapping_guard
					|| (has_fragment_reader
						&& !prepared_group->persisted_fragment_reader->ready())
					|| (has_grouped_soa_reader
						&& !prepared_group->
							persisted_grouped_soa_reader->ready())
					|| prepared_group->target_index
					|| prepared_group->target_index_source
						!= ::bioseq::pipeline_stats::target_index_source::persisted
					|| prepared_group->persisted_generation_id == 0
					|| prepared_group->persisted_source_identity == 0
					|| !hash_major_identity_valid
					|| !canonical_owner_exclusive
					|| !hash_major_read_receipt_valid) {
					raise_required_persisted_index_error(
						scanner_pg_ctx,
						scanner_pg_ctx->target_rownos[i],
						"the exact READY native persisted reader is absent, incomplete, duplicated, or "
						"mixed with a storage-profile-transcoded assembled index");
					}
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
											query_bp_size)
									|| !pipe::minimizer_range_stats_matches_backend(
										backend,
										retained_prepared_query_sketch->execution_stats,
										/*require_nonempty_vector_work=*/false,
										pipe::exact_simd_range_route::
											require_continuation)) {
									raise_typed_dispatch_pg_error_as_cpp(
										ERRCODE_DATA_CORRUPTED,
										"retained native query sketch stats do not "
										"match the selected minimizer backend");
								}
								row_consumed_persisted_query_sketch = true;
							} else {
								ensure_q();
							}
							const std::vector<minimap_sketch_data>&
								alignment_query_minimizers =
									retained_prepared_query_sketch != nullptr
									? retained_prepared_query_sketch->minimizers
									: *query_minimizers;
							if (has_hash_major_reader) {
							collection = minimap_port_hash_major_pg_checked(
								fq_pg_view,
								ft_pg_view,
							alignment_query_minimizers,
							*prepared_group->persisted_hash_major_reader,
								idx_params,
								/*gpu_index_cache=*/nullptr,
								bioseq::indexing::gpu_index_cache_identity{});
							} else if (has_fragment_reader) {
							collection = run_aligner_pg_checked(
								"fragment-native single-query anchor preparation",
									[&alignment_query_minimizers, query_bp_size,
									 &idx_params, prepared_group, fq_pg_view,
									 ft_pg_view]() -> match_result_collection {
									const std::uint64_t prepare_started_ns =
										dispatch_host_now_ns();
									auto& admission = bioseq::parallelism::
										memory_admission_controller::instance();
									auto scratch_result = bioseq::indexing::
										query_wave_scratch::create(admission);
									if (!scratch_result) {
										throw bioseq::indexing::
											persisted_minimizer_lookup_error(
												"fragment-native query scratch was not "
												"admitted; wire_error="
												+ std::to_string(static_cast<unsigned>(
													scratch_result.error())));
									}
									auto scratch = std::move(*scratch_result);
					const bioseq::indexing::fragment_seed_query request{
						.minimizers = alignment_query_minimizers,
						.query_length = static_cast<sequence_size_t>(
							query_bp_size),
						.kmer_size = idx_params.kmer_size,
									};
									fragment_anchor_wave_completion wave_completion{
										scratch};
									auto prepared = prepared_group->
										persisted_fragment_reader->
										prepare_fragment_anchor_batch(
											admission,
											std::span<const bioseq::indexing::
												fragment_seed_query>{&request, 1u},
											idx_params.max_occ,
											scratch,
											/*baseline_query_plan_matcher_for_testing=*/false,
											/*verify_compositional_order_for_testing=*/false,
											prepared_group->
												persisted_fragment_lookup_route);
									const auto query_index = prepared.query_view(0u);
									const std::uint64_t prepare_host_wall_ns =
										elapsed_dispatch_host_ns(prepare_started_ns);
									match_result_collection out =
										minimap_port_fragment_query_owned(
										fq_pg_view,
										ft_pg_view,
										alignment_query_minimizers,
										query_index,
										idx_params,
										/*gpu_index_cache=*/nullptr,
										bioseq::indexing::gpu_index_cache_identity{});
										out.record_seed_lookup_host_time(
											prepare_host_wall_ns);
										out.record_fragment_native_query_stats(
											prepared.stats());
										return out;
								});
							} else {
							collection = minimap_port_grouped_soa_pg_checked(
								fq_pg_view,
								ft_pg_view,
								alignment_query_minimizers,
								*prepared_group->
									persisted_grouped_soa_reader,
								idx_params,
								/*gpu_index_cache=*/nullptr,
								bioseq::indexing::gpu_index_cache_identity{});
							}
				if (prepared_group->persisted_reader_queries == 0u
					&& prepared_group->persisted_reader_load_host_wall_ns != 0u) {
					collection.record_persisted_index_decode_host_time(
						prepared_group->persisted_reader_load_host_wall_ns);
				}
				++prepared_group->persisted_reader_queries;
				assembled = true;
				scanner_pg_ctx->target_index_sources[i] =
					::bioseq::pipeline_stats::target_index_source::persisted;
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
				collection = minimap_port_fragment_index_vector_pg_checked(
					fq_pg_view,
					ft_pg_view,
					*query_minimizers,
					*prepared_group->target_index,
					idx_params,
					/*gpu_index_cache=*/nullptr,
					bioseq::indexing::gpu_index_cache_identity{
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
                const auto query_span = std::span<const minimap_sketch_data>(
                    query_minimizers->data(), query_minimizers->size());
                const auto sketch_backend =
                    pipe::target_index_sketch_backend_from_params(idx_params);
                return bioseq::storage::indexing::assembled_index_key{
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
                         query_bp_size, &query_minimizers,
                         &collection, &assembled,
                         &make_cache_key, fq_pg_view, ft_pg_view,
		                 target_letters_per_unit,
		                 pg_gpu_index_cache_ptr, prepared_group,
		                 cpu_prepared_target](
                                index_storage_type& fragment_store,
		                                            std::string_view cache_index_name) {
	                                    ensure_q();
	                                    const sequence_size_t total_bp =
	                                        loc_opt->logical_length;
	                                    const std::size_t a_lpu =
	                                        target_letters_per_unit;
	                                    constexpr sequence_size_t frag_bp =
	                                        static_cast<sequence_size_t>(
	                                            bioseq::storage::
	                                                postgres_lazy_sequence_binding<
	                                                    p_sequence>::
	                                                    fragment_bp_capacity());
	                                    static_assert(frag_bp != 0u);
	                                    const std::size_t size_u =
	                                        static_cast<std::size_t>(
	                                            (total_bp + a_lpu - 1) / a_lpu);
	                                    bioseq::indexing::minimizer_assembly_scratch scratch;
	                                    const auto& assembly_minimizers =
	                                        prepared_group == nullptr
	                                            ? *query_minimizers
	                                            : prepared_group->query_hash_union;
	                                    auto assemble_one =
	                                        [a_lpu, total_bp, frag_bp,
	                                         &fragment_store, &scratch,
	                                         &assembly_minimizers](
	                                                std::size_t cs_u,
	                                                std::size_t ce_u) {
	                                        const sequence_size_t cs_bp =
	                                            static_cast<sequence_size_t>(cs_u) * a_lpu;
	                                        const sequence_size_t ce_bp =
	                                            std::min(
	                                                static_cast<sequence_size_t>(ce_u) * a_lpu,
	                                                total_bp);
	                                        const std::size_t frag_lo =
	                                            static_cast<std::size_t>(
	                                                cs_bp / frag_bp);
	                                        const std::size_t frag_hi =
	                                            static_cast<std::size_t>(
	                                                bioseq::common::ceil_div(
	                                                    ce_bp, frag_bp));
	                                        const auto query_span =
	                                            std::span<const minimap_sketch_data>(
	                                                assembly_minimizers.data(),
	                                                assembly_minimizers.size());
	                                        return bioseq::indexing::minimizers_per_fragment_index<
	                                            index_storage_type>::assemble_segment_for_query(
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
	                                            return minimap_port_fragment_index_vector_pg_checked(
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
		                                                bioseq::parallelism::memory_admission_controller::instance()
		                                                    .try_reserve(admission_bytes);
		                                            if (!permit) {
		                                                raise_typed_dispatch_pg_error_as_cpp(
		                                                    ERRCODE_OUT_OF_MEMORY,
		                                                    "bioseq batch join prepared persisted target "
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
	                                                        bioseq::indexing::minimizer_hashmap_fragment_index>(
	                                                            std::move(idx));
	                                                prepared_group->target_index_source =
	                                                    ::bioseq::pipeline_stats::target_index_source::persisted;
	                                                ++prepared_group->target_index_builds;
	                                                prepared_group->build_set_checksum_present = true;
	                                                prepared_group->build_set_checksum =
	                                                    prepared_group->key_set_checksum;
	                                                ++cpu_prepared_target->physical_target_index_reads;
	                                            }
	                                            return minimap_port_fragment_index_vector_pg_checked(
	                                                fq_pg_view,
	                                                ft_pg_view,
	                                                *query_minimizers,
	                                                *prepared_group->target_index,
	                                                idx_params,
	                                                /*gpu_index_cache=*/nullptr,
	                                                bioseq::indexing::gpu_index_cache_identity{
	                                                    .assembly_identity =
	                                                        prepared_group->key_set_checksum});
	                                        }
	                                        match_result_collection out =
	                                            minimap_port_fragment_index_vector_pg_checked(
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
	                                    } else if (total_bp <= postgres_segmentation_threshold_bp) {
	                                        collection = run_with_segment_index(
	                                            0, total_bp, 0, size_u);
	                                    } else {
	                                        match_result_collection merged_collection;
	                                        std::vector<match_result>& merged =
	                                            merged_collection.results;
	                                        const sequence_size_t query_len =
	                                            query_bp_size;
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
	                                            match_result_collection seg_res =
	                                                run_with_segment_index(
	                                                    clip_lo_bp,
	                                                    clip_hi_bp,
	                                                    sr.cs_u,
	                                                    sr.ce_u);
	                                            merged_collection.combine_execution_stats(
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
                                // Route by the table's ACTIVE read index.
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
	                                            minimizers_per_sequence_index_name,
	                                            *loc_opt,
		                                            static_cast<std::uint64_t>(
		                                                scanner_pg_ctx->target_rownos[i]),
		                                            scanner_pg_ctx->caches,
		                                            scanner_pg_ctx->parent_mctx);
		                                    with_checked_minimizer_fragment_reader(
		                                        seq_store, [&] {
		                                    if (seq_store.number_of_chunks(
	                                            bioseq::indexing::k_directory_sub_key) >= 1) {
	                                        const std::size_t dir_bytes =
	                                            seq_store.chunk_byte_size(
	                                                bioseq::indexing::k_directory_sub_key,
	                                                0);
	                                        if (dir_bytes == sizeof(std::int64_t)) {
	                                            run_fragment_backed_index(
	                                                seq_store,
	                                                minimizers_per_sequence_index_name);
	                                            bioseq_log<logging_level::debug>("[bioseq] index: logical fragment-backed read index=minimizers_per_sequence row={} target_bp={}",
	                                                    static_cast<unsigned long long>(
	                                                        scanner_pg_ctx->target_rownos[i]),
	                                                    static_cast<unsigned long long>(
	                                                        loc_opt->logical_length));
	                                        } else {
	                                            ensure_q();
	                                            const sequence_size_t total_bp =
	                                                loc_opt->logical_length;
	                                            const std::size_t a_lpu =
	                                                target_letters_per_unit;
	                                            const std::vector<minimap_sketch_data>& query_vec =
	                                                *query_minimizers;
	                                            const std::vector<minimap_sketch_data>& assembly_query_vec =
	                                                prepared_group == nullptr
	                                                    ? query_vec
	                                                    : prepared_group->query_hash_union;
	                                            const auto query_span =
	                                                std::span<const minimap_sketch_data>(
	                                                    assembly_query_vec.data(),
	                                                    assembly_query_vec.size());
	                                            bioseq::indexing::minimizer_assembly_scratch sq_scratch;
	                                            auto run_minimizer_sequence_index =
	                                                [&make_cache_key, &seq_store,
	                                                 fq_pg_view, ft_pg_view,
	                                                 &idx_params, &query_vec, query_span, total_bp,
	                                                 &sq_scratch, pg_gpu_index_cache_ptr,
	                                                 prepared_group,
	                                                 cpu_prepared_target](
	                                                        sequence_size_t clip_lo_bp,
	                                                        sequence_size_t clip_hi_bp) {
	                                                const auto cache_key = make_cache_key(
	                                                    minimizers_per_sequence_index_name,
	                                                    clip_lo_bp,
	                                                    clip_hi_bp);
	                                                const auto gpu_identity =
	                                                    pg_gpu_index_identity(cache_key);
	                                                if (prepared_group == nullptr) if (auto hit =
	                                                        bioseq::storage::indexing::lookup_assembled_index(
	                                                            cache_key)) {
	                                                    return minimap_port_fragment_index_vector_pg_checked(
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
		                                                        bioseq::parallelism::memory_admission_controller::instance()
		                                                            .try_reserve(admission_bytes);
		                                                    if (!permit) {
		                                                        raise_typed_dispatch_pg_error_as_cpp(
		                                                            ERRCODE_OUT_OF_MEMORY,
		                                                            "bioseq batch join prepared persisted target "
		                                                            "index cannot be admitted");
	                                                    }
	                                                    prepared_group->target_index_permit.emplace(
	                                                        std::move(*permit));
	                                                }
	                                                auto idx = bioseq::indexing::direct_minimizer_bucket_index<
	                                                    index_storage_type>::assemble_for_query_segment(
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
	                                                                bioseq::indexing::minimizer_hashmap_fragment_index>(
	                                                                    std::move(idx));
	                                                        prepared_group->target_index_source =
	                                                            ::bioseq::pipeline_stats::target_index_source::persisted;
	                                                        ++prepared_group->target_index_builds;
	                                                        prepared_group->build_set_checksum_present = true;
	                                                        prepared_group->build_set_checksum =
	                                                            prepared_group->key_set_checksum;
	                                                        ++cpu_prepared_target->physical_target_index_reads;
	                                                    }
	                                                    return minimap_port_fragment_index_vector_pg_checked(
	                                                        fq_pg_view,
	                                                        ft_pg_view,
	                                                        query_vec,
	                                                        *prepared_group->target_index,
	                                                        idx_params,
	                                                        /*gpu_index_cache=*/nullptr,
	                                                        bioseq::indexing::gpu_index_cache_identity{
	                                                            .assembly_identity =
	                                                                prepared_group->key_set_checksum});
	                                                }
	                                                match_result_collection out =
	                                                    minimap_port_fragment_index_vector_pg_checked(
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
	                                                collection =
	                                                    run_minimizer_sequence_index(0, total_bp);
	                                            } else if (total_bp <= postgres_segmentation_threshold_bp) {
	                                                collection =
	                                                    run_minimizer_sequence_index(0, total_bp);
	                                            } else {
	                                                match_result_collection merged_collection;
	                                                std::vector<match_result>& merged =
	                                                    merged_collection.results;
	                                                const sequence_size_t query_len =
	                                                    query_bp_size;
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
	                                                    match_result_collection seg_res =
	                                                        run_minimizer_sequence_index(
	                                                            clip_lo_bp, clip_hi_bp);
	                                                    merged_collection.combine_execution_stats(
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
	                                            minimizers_per_fragment_index_name,
	                                            *loc_opt,
		                                            static_cast<std::uint64_t>(
		                                                scanner_pg_ctx->target_rownos[i]),
		                                            scanner_pg_ctx->caches,
		                                            scanner_pg_ctx->parent_mctx);
		                                    with_checked_minimizer_fragment_reader(
		                                        frag_store, [&] {
		                                    if (frag_store.number_of_chunks(
	                                            bioseq::indexing::k_directory_sub_key) >= 1) {
	                                        run_fragment_backed_index(
		                                            frag_store,
		                                            minimizers_per_fragment_index_name);
		                                    }
		                                        });
		                                }
	                                }
            if (assembled) {
                scanner_pg_ctx->target_index_sources[i] =
                    ::bioseq::pipeline_stats::target_index_source::persisted;
            }
            if (persisted_required && !assembled) {
                raise_required_persisted_index_error(
                    scanner_pg_ctx,
                    scanner_pg_ctx->target_rownos[i],
                    "the active persisted minimizer index has no compatible "
                    "directory/chunks for this target row");
            }
            // A recompute under an
            // An active per-sequence read index with no persisted minimizer-sequence row
            // must build a real transient minimizer-sequence index via the streaming GPU
            // sorted-pair seam, not relabel an opportunistic minimizer-fragment index.
            // The same invariant applies to both the
            // asynchronous/deferred route and active-read recomputation.
            if (!assembled && want_seq) {
                collection = build_and_align_transient_minimizer_sequence(
                    fq_pg_view,
                    ft_pg_view,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->logical_length),
                    target_letters_per_unit);
                assembled = true;
                bioseq_log<logging_level::debug>("[bioseq] index: recompute real-minimizer-sequence-read-index row={} target_bp={} backend={}",
                        static_cast<unsigned long long>(
                            scanner_pg_ctx->target_rownos[i]),
                        static_cast<unsigned long long>(
                            loc_opt->logical_length),
                        bioseq::minimizers::minimizer_sketch_backend_name(
                            target_backend_from_params_for_recompute(params)));
            }
	                                if (!assembled
	                                    && loc_opt->logical_length >= opportunistic_index_minimum_bp) {
                const std::uint64_t peak_bytes =
                    bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                        static_cast<std::uint64_t>(loc_opt->logical_length));
                auto build_permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(peak_bytes);
                if (build_permit) {
                    if (params.highway_kernels == nullptr) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_INTERNAL_ERROR,
                            "opportunistic minimizer-fragment build has no selected Highway minimizer table");
                    }
                    ensure_q();
                    opportunistic_fragment_index_storage ram_store;
	                                        bioseq::indexing::minimizers_per_fragment_index<
	                                            opportunistic_fragment_index_storage>::build(
	                                                ram_store, ft_pg_view,
                            target_backend_from_params_for_recompute(params),
                            &params.highway_kernels->minimizers,
                            params.alphabet_code);
	                                        const std::string_view opportunistic_index_name =
	                                            want_seq
	                                                ? minimizers_per_sequence_index_name
	                                                : minimizers_per_fragment_index_name;

	                                        bioseq_log<logging_level::debug>("[bioseq] index: opportunistic RAM-first index row={} index={} target_bp={} chunks={} peak_bytes={}",
	                                                static_cast<unsigned long long>(
	                                                    scanner_pg_ctx->target_rownos[i]),
	                                                opportunistic_index_name,
	                                                static_cast<unsigned long long>(
	                                                    loc_opt->logical_length),
	                                                ram_store.chunks.size(),
                            static_cast<unsigned long long>(peak_bytes));

                    const sequence_size_t total_bp = loc_opt->logical_length;
                    const std::size_t a_lpu = target_letters_per_unit;
                    constexpr sequence_size_t frag_bp =
                        static_cast<sequence_size_t>(
                            bioseq::storage::
                                postgres_lazy_sequence_binding<p_sequence>::
                                fragment_bp_capacity());
                    static_assert(frag_bp != 0u);
                    const std::size_t size_u =
                        static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                    bioseq::indexing::minimizer_assembly_scratch scratch;
                    auto assemble_ram_one =
                        [a_lpu, total_bp, frag_bp,
                         &ram_store, &scratch, &query_minimizers](
                                std::size_t cs_u,
                                std::size_t ce_u) {
                        const sequence_size_t cs_bp =
                            static_cast<sequence_size_t>(cs_u) * a_lpu;
                        const sequence_size_t ce_bp =
                            std::min(
                                static_cast<sequence_size_t>(ce_u) * a_lpu,
                                total_bp);
                        const std::size_t frag_lo =
                            static_cast<std::size_t>(cs_bp / frag_bp);
                        const std::size_t frag_hi =
                            static_cast<std::size_t>(
                                bioseq::common::ceil_div(ce_bp, frag_bp));
                        const auto query_span =
                            std::span<const minimap_sketch_data>(
                                query_minimizers->data(),
                                query_minimizers->size());
                        return bioseq::indexing::minimizers_per_fragment_index<
                            opportunistic_fragment_index_storage>::assemble_segment_for_query(
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
                            != bioseq::indexing::index_read_requirement::
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
                                return minimap_port_fragment_index_vector_pg_checked(
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
                        match_result_collection out =
                            minimap_port_fragment_index_vector_pg_checked(
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
                    if (total_bp <= postgres_segmentation_threshold_bp) {
                        collection = run_with_ram_index(
                            0, total_bp, 0, size_u);
                    } else {
                        match_result_collection merged_collection;
                        std::vector<match_result>& merged =
                            merged_collection.results;
                        const sequence_size_t query_len = query_bp_size;
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
                            match_result_collection seg_res =
                                run_with_ram_index(
                                    clip_lo_bp,
                                    clip_hi_bp,
                                    sr.cs_u,
                                    sr.ce_u);
                            merged_collection.combine_execution_stats(seg_res);
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
                    bioseq_log<logging_level::debug>("[bioseq] index: opportunistic RAM-first index skipped row={} target_bp={} peak_bytes={} (not admitted)",
                            static_cast<unsigned long long>(
                                scanner_pg_ctx->target_rownos[i]),
                            static_cast<unsigned long long>(
                                loc_opt->logical_length),
                            static_cast<unsigned long long>(peak_bytes));
                }
            }
        }
        // Normal allow/no-index mode may recompute without manufacturing or consuming
        // a scheme-less minimizer-sequence representation. The transient builder is
        // available only for a supported stored minimizer scheme.
        const bool want_seq_noindex =
            scanner_pg_ctx->active_index == minimizers_per_sequence_index_name
            && stored_minimizer_scheme_compatible;
        if (!assembled && want_seq_noindex) {
            const auto rowno =
                static_cast<std::uint64_t>(
                    scanner_pg_ctx->target_rownos[i]);
            // No-index minimizer-sequence must use a real transient
            // minimizers_per_sequence index even when
            // index.recompute_async_persistence.enabled is off. Reads are
            // disabled here, but the active policy still
            // selects the algorithm shape.
            collection = build_and_align_transient_minimizer_sequence(
                fq_pg_view,
                ft_pg_view,
                params,
                static_cast<sequence_size_t>(
                    loc_opt->logical_length),
                target_letters_per_unit);
			// build_and_align_transient_minimizer_sequence always builds and queries the
			// persisted minimizer-sequence scheme. Pass that concrete scheme to staging even
			// when the short-query heuristic changed the outer alignment pair;
			// ordinary streaming callers retain their requested k/w and still
			// fail closed in can_defer_recompute_minimizer_persist().
			minimap2_parameters minimizer_sequence_stage_params = params;
			minimizer_sequence_stage_params.kmer_size = 15;
			minimizer_sequence_stage_params.window_size = 10;
			const bool async_staged =
				stage_recompute_minimizer_sequence_build_if_selected(
					*scanner_pg_ctx, *loc_opt, rowno,
					alphabet_code, storage_unit_byte_width,
					static_cast<std::uint16_t>(_padding),
					minimizer_sequence_stage_params,
					"real_minimizer_sequence_active_policy_noindex");
			bioseq_log<logging_level::debug>("[bioseq] index: recompute real-minimizer-sequence-noindex-policy row={} target_bp={} async_staged={} persisted=false backend={}",
                    static_cast<unsigned long long>(rowno),
                    static_cast<unsigned long long>(
                        loc_opt->logical_length),
					async_staged,
                    bioseq::minimizers::minimizer_sketch_backend_name(
                        target_backend_from_params_for_recompute(params)));
            assembled = true;
        }
        if (!assembled) {
            const bool can_defer_fast_minimizer_sequence_recompute =
                can_defer_recompute_minimizer_persist(
                    *scanner_pg_ctx, params);
            const auto fast_minimizer_sequence_choice =
				can_defer_fast_minimizer_sequence_recompute
                    ? choose_deferred_minimizer_persist_index(
                        *scanner_pg_ctx)
                    : std::optional<deferred_minimizer_persist_choice>{};
            if (fast_minimizer_sequence_choice.has_value()
				&& fast_minimizer_sequence_choice->kind
                    == deferred_minimizer_index_kind::per_sequence) {
                const auto rowno =
                    static_cast<std::uint64_t>(
                        scanner_pg_ctx->target_rownos[i]);
                // Build a real transient minimizer-sequence index for the
                // no-index/per-sequence cell. The GPU sorted-pair seam (GPU emit + host
                // sort) streams from lazy PostgreSQL (no whole-target
                // resident materialization, so 2/4 Gbp targets do not OOM).
                collection = build_and_align_transient_minimizer_sequence(
                    fq_pg_view,
                    ft_pg_view,
                    params,
                    static_cast<sequence_size_t>(
                        loc_opt->logical_length),
                    target_letters_per_unit);
				const bool async_staged =
					stage_recompute_minimizer_sequence_build_if_selected(
						*scanner_pg_ctx, *loc_opt, rowno,
						alphabet_code, storage_unit_byte_width,
						static_cast<std::uint16_t>(_padding),
						params, "real_minimizer_sequence_fast_noindex");
				bioseq_log<logging_level::debug>("[bioseq] index: recompute real-minimizer-sequence-fast-noindex row={} index={} target_bp={} async_staged={} persisted=false backend={}",
                        static_cast<unsigned long long>(
                            rowno),
                        fast_minimizer_sequence_choice->index_name,
                        static_cast<unsigned long long>(
                            loc_opt->logical_length),
						async_staged,
                        bioseq::minimizers::minimizer_sketch_backend_name(
                            target_backend_from_params_for_recompute(params)));
                assembled = true;
            }
        }
        if (!assembled) {
            const bool try_resident_segment_workers =
                target_bp_size > postgres_segmentation_threshold_bp
                && !dispatch_params.enable_profiling
                && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
                && pool.maximum_concurrency() > 1;
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
                            : std::optional<deferred_minimizer_persist_choice>{};
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
                            bioseq_log<logging_level::debug>("[bioseq] index: recompute async-persist skipped row={} target_bp={} because active params are k={} w={} but persisted minimizer indexes are pinned to k=15 w=10",
                                    static_cast<unsigned long long>(
                                        scanner_pg_ctx->target_rownos[i]),
                                    static_cast<unsigned long long>(
                                        loc_opt->logical_length),
                                    params.kmer_size,
                                    params.window_size);
                        } else if (can_defer_recompute_persist) {
                            bioseq_log<logging_level::debug>("[bioseq] index: recompute async-persist skipped row={} target_bp={} because no minimizer index is enabled for UPDATE_ON_INSERT",
                                    static_cast<unsigned long long>(
                                        scanner_pg_ctx->target_rownos[i]),
                                    static_cast<unsigned long long>(
                                        loc_opt->logical_length));
                        }
                    }
                } else {
                    collection = minimap_port_segmented_pg_checked(
                        fq_pg_view, ft_pg_view, params);
                    stage_recompute_minimizer_sequence_build_if_selected(
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
                    (target_bp_size > postgres_segmentation_threshold_bp)
                        ? minimap_port_segmented_pg_checked(
                            fq_pg_view, ft_pg_view, params)
                        : minimap_port_raw_pg_checked(
                            fq_pg_view, ft_pg_view, params);
                stage_recompute_minimizer_sequence_build_if_selected(
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
        compact_sequence_range<p_sequence> target_seq{*target_hdr};
        const auto target_view =
            bioseq::storage::make_lazy_sequence_view(target_seq);
        collection =
            (target_bp_size > postgres_segmentation_threshold_bp)
                ? minimap_port_segmented_pg_checked(
                    lazy_query_view, target_view, params)
                : minimap_port_raw_pg_checked(
                    lazy_query_view, target_view, params);
    }

    if (query_sketch_host_wall_ns != 0u) {
        collection.record_query_sketch_host_time(
            query_sketch_host_wall_ns);
    }
    if (row_consumed_persisted_query_sketch) {
		const pipe::minimizer_range_execution_stats* completed_query_stats =
			retained_prepared_query_sketch != nullptr
				? &retained_prepared_query_sketch->execution_stats
				: per_row_query_mode
					? row_query_minimizer_stats
						? &*row_query_minimizer_stats
						: nullptr
					: cached_query_minimizer_stats
						? &*cached_query_minimizer_stats
						: nullptr;
		if (completed_query_stats == nullptr) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_DATA_CORRUPTED,
				"consumed persisted query minimizers have no operation stats");
        }
        const bool exact_query_stats_absent =
            !collection.exact_simd_query_execution_stats_available
            && collection.exact_simd_query_execution_stats
                == decltype(collection.exact_simd_query_execution_stats){};
		const bool scalar_query_stats_absent =
			collection.scalar_pipeline_stats.query_minimizers
				== decltype(collection.scalar_pipeline_stats
					.query_minimizers){};
		const bool bounded_query_stats_absent =
			!collection.bounded_nthash_query_execution_stats_available
			&& collection.bounded_nthash_query_execution_stats
				== decltype(collection.bounded_nthash_query_execution_stats){};
		if (!scalar_query_stats_absent
			|| !exact_query_stats_absent
			|| !bounded_query_stats_absent) {
			raise_typed_dispatch_pg_error_as_cpp(
				ERRCODE_INTERNAL_ERROR,
				"persisted query sketch stats were recorded more than once");
        }
        if (completed_query_stats->exact_simd_available) {
            collection.record_exact_simd_query_execution_stats(
                completed_query_stats->exact_simd);
        }
        if (completed_query_stats->bounded_nthash_available) {
            collection.record_bounded_nthash_query_execution_stats(
                completed_query_stats->bounded_nthash);
        }
        if (completed_query_stats->scalar_available) {
            collection.record_scalar_query_minimizer_execution_stats(
                completed_query_stats->scalar);
        }
        consumed_shared_cached_query_stats = !per_row_query_mode;
    }
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[nodiscard]] match_result_collection process_inmemory_standard_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_raw_pg_checked(
        lazy_query_view, lazy_target_view, params);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_segmented_pg_checked(
        lazy_query_view, lazy_target_view, params);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_raw_pg_checked(
                query_view, target_view, params);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_segmented_pg_checked(
                query_view, target_view, params);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory")]]
[[nodiscard]] match_result_collection process_inmemory_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const sequence_size_t target_bp_size,
    const minimap2_parameters& params,
    const bool chunked_scanner_forced)
{
    const bool segmented = target_bp_size > postgres_segmentation_threshold_bp;
    if (chunked_scanner_forced) {
        return segmented
            ? process_inmemory_chunked_segmented_row(
                row_query_hdr, target_hdr, params)
            : process_inmemory_chunked_row(
                row_query_hdr, target_hdr, params);
    }
    return segmented
        ? process_inmemory_segmented_row(
            row_query_hdr, target_hdr, params)
        : process_inmemory_standard_row(
            row_query_hdr, target_hdr, params);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "root")]]
[[nodiscard]] std::map<int32_t, std::vector<match_result>> process_sequences_typed(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& base_params,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<match_result>> results;
    const std::size_t facade_letters_per_unit =
        bioseq::storage::packed_letters_per_unit(
            storage_unit_byte_width,
            static_cast<std::uint8_t>(p_sequence::letter_bit_width));
    if (facade_letters_per_unit == 0u) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "typed batch facade has invalid packed storage geometry");
    }
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;
    const bool record_target_operational_outcomes =
        invocation_role == batch_invocation_role::primary
        && (scanner_pg_ctx == nullptr
            || scanner_pg_ctx->record_target_operational_outcomes);
	bool shared_target_outcome_recorded = false;

    const minimap2_parameters& dispatch_params = base_params;

	    const std::optional<std::string> chunked_scanner_guc =
	        pg_guc_value_as_cpp(
	            "bioseq.testing.chunked_scanner.enabled");
	    const bool chunked_scanner_forced =
	        chunked_scanner_guc.has_value()
	        && *chunked_scanner_guc == "on";
            // Test parity knob. When on, the dispatch
            // loop forces the in-memory baseline (with_lazy_in_memory). Otherwise
            // (default) and given a non-null scanner_pg_context, the loop uses
            // the canonical PostgreSQL-backed facade (for stub targets,
            // in-memory for the query literal / inline targets).
	    const std::optional<std::string> inmemory_scanner_guc =
	        pg_guc_value_as_cpp(
	            "bioseq.testing.in_memory_scanner.enabled");
	    const bool inmemory_scanner_forced =
	        inmemory_scanner_guc.has_value()
	        && *inmemory_scanner_guc == "on";
            bool scanner_has_recipe = false;
            if (scanner_pg_ctx != nullptr) {
                for (std::size_t row = 0u;
                     row < batch_size && !scanner_has_recipe; ++row) {
                    scanner_has_recipe = scanner_pg_ctx->
                            target_recipe_datums[row] != Datum{0}
                        || scanner_pg_ctx->query_recipe_datums[row]
                            != Datum{0};
                }
            }
            const bool use_scanner_pg =
                scanner_pg_ctx != nullptr
                && ((!inmemory_scanner_forced
                        && !chunked_scanner_forced)
                    || scanner_has_recipe);
            const auto index_read_requirement =
                scanner_pg_ctx != nullptr
                    ? scanner_pg_ctx->index_read_requirement
                    : bioseq::indexing::index_read_requirement::allow;
            const bool persisted_required =
                index_read_requirement
                == bioseq::indexing::index_read_requirement::
                    require_compatible_persisted;
			if (scanner_pg_ctx != nullptr) {
				using policy_source_type = bioseq::storage::indexing::
					active_read_policy_source;
				const policy_source_type source =
					scanner_pg_ctx->active_index_policy_source;
				const bool relation_backed =
					OidIsValid(scanner_pg_ctx->user_relid);
				const bool valid_relation_source = relation_backed
					&& (source == policy_source_type::configured_row
						|| source
							== policy_source_type::authoritative_no_row_default);
				const bool valid_non_relation_source = !relation_backed
					&& source == policy_source_type::non_relation;
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
						"active-read policy dispatch stats are invalid",
						diagnostic.c_str());
				}
			}
            if (index_read_requirement
					== bioseq::indexing::index_read_requirement::require_absent
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
				&& scanner_pg_ctx->gpu_prepared_grouped_soa_target == nullptr;
			const bool gpu_persisted_prepared = scanner_pg_ctx != nullptr
				&& scanner_pg_ctx->cpu_prepared_target == nullptr
				&& scanner_pg_ctx->gpu_prepared_grouped_soa_target != nullptr
				&& scanner_pg_ctx->gpu_prepared_grouped_soa_target->ready();
            if (persisted_required
				&& (selected_vulkan
					? !gpu_persisted_prepared : !cpu_persisted_prepared)) {
                raise_required_persisted_index_error(
                    scanner_pg_ctx,
                    -1,
					selected_vulkan
						? "GPU execution requires one retained authenticated grouped-SoA target and forbids a CPU prepared target"
						: "ordinal 2 CPU execution requires one retained native persisted reader and forbids a GPU prepared target");
            }
			bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor dispatching ({},{}) query_bp_size={} batch_size={} per_row_query={} use_scanner_pg={} chunked={} inmemory_forced={} active_index={} active_index_policy_source={}",
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
             * Recompute-path rows use minimap_port_raw_pg_checked(), which does its
             * own sketching; the extra allocation here is avoided
             * for those rows via the optional nested-scope.
             */
            std::optional<std::vector<minimap_sketch_data>> cached_query_minimizers;
            std::optional<pipe::minimizer_range_execution_stats>
                cached_query_minimizer_stats;
            bool cached_query_minimizer_stats_accounted = false;
            constexpr std::size_t gpu_batch_minimum_rows = 2;
			bool persisted_transaction_active = false;

            if (selected_vulkan_execution(dispatch_params)
                && !pipe::complete_all_gpu_tuple_selected(dispatch_params)) {
				add_operational_counter(
					stats, stats.operational.backend_declines);
                bioseq::gpu::vulkan::vulkan_runtime_error err{
                    "requested Vulkan components do not form the complete "
                    "supported all-GPU tuple; generic CPU/SIMD biological "
                    "fallback is forbidden"};
                raise_vulkan_pg_error(
                    "Vulkan complete-tuple admission", err.what());
            }

            if (selected_vulkan_execution(dispatch_params)
                && batch_size < gpu_batch_minimum_rows) {
				add_operational_counter(
					stats, stats.operational.backend_declines);
                bioseq::gpu::vulkan::vulkan_runtime_error err{
                    "selected Vulkan execution received a one-job wave; "
                    "silent CPU/SIMD fallthrough is forbidden"};
                raise_vulkan_pg_error("Vulkan batch cardinality", err.what());
            }

            if (selected_vulkan_execution(dispatch_params)
                       && batch_size >= gpu_batch_minimum_rows
                       && !chunked_scanner_forced) {
                const bool selected_vulkan_bounded_cigar =
                    dispatch_params.do_cigar
                    && dispatch_params.vulkan_extend_enabled
                    && selected_vulkan_coordinate_prereqs_active(dispatch_params);
                if ((!pipe::is_gpu_coordinate_only_execution_active(dispatch_params)
                     && !selected_vulkan_bounded_cigar)
                    || !pipe::is_gpu_target_index_build_active(dispatch_params)) {
					add_operational_counter(
						stats, stats.operational.backend_declines);
                    bioseq::gpu::vulkan::vulkan_runtime_error err{
                        "selected Vulkan batch requires coordinate-only projection "
                        "or bounded GPU CIGAR extension, "
                        "Vulkan minimizers, Vulkan tiled-DP chaining, fused frontend, "
                        "and GPU target-index build"};
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }

                auto& runtime =
                    bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
                if (!runtime.is_usable()) {
					add_operational_counter(
						stats, stats.operational.backend_declines);
                    bioseq::gpu::vulkan::vulkan_runtime_error err{
                        "selected Vulkan batch runtime unavailable: "
                        + runtime.initialization_error()};
                    raise_vulkan_pg_error(
                        "Vulkan batched resident coordinate", err.what());
                }
				if (gpu_persisted_prepared
					&& (dispatch_params.do_cigar
						|| !pipe::is_gpu_coordinate_only_execution_active(
							dispatch_params))) {
					raise_typed_dispatch_pg_error_as_cpp(
						ERRCODE_FEATURE_NOT_SUPPORTED,
						"persisted GPU grouped-SoA execution does not yet support CIGAR projection",
						"the direct path can execute query sketching, persisted Shape lookup, "
						"GPU chaining, backtracking, and coordinate projection, but no genuine "
						"GPU target-window reader is wired for alignment extension",
						"Use coordinate-only projection; CPU target hydration or SIMD/CPU "
						"alignment fallback is intentionally forbidden.");
				}

                using minimizer_type = minimap_sketch_data;
                using gpu_row_alphabet = typename
                    compact_sequence_range<p_sequence>::traits::alphabet_type;
                using gpu_row_view = bioseq::storage::
                    lazy_sequence_view<gpu_row_alphabet>;
                using gpu_admitted_owner = bioseq::storage::
                    admitted_packed_sequence<gpu_row_alphabet>;
                struct gpu_batch_query_sidecar {
                    std::size_t row_index = 0;
                    sequence_size_t query_len = 0;
                };
                struct gpu_batch_target_sidecar {
                    const bioseq::pg_types::seq* hdr = nullptr;
                    std::optional<gpu_row_view> recipe_view{};
                    Datum recipe_datum = Datum{0};
                    std::size_t source_row_index = 0u;
                    std::uint64_t fingerprint = 0;
                    sequence_size_t target_len = 0;
                    sequence_size_t max_query_len = 0;
                    std::size_t row_count = 0;
                    std::vector<::bioseq::indexing::segment_unit_range> ranges{};
                    std::vector<std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>>
                        residents{};
                };
                struct gpu_batch_problem_sidecar {
                    std::size_t row_index = 0;
                    std::size_t target_index = 0;
                    std::size_t segment_index = 0;
                    sequence_size_t core_lo_bp = 0;
                    sequence_size_t core_hi_bp = 0;
                };

                try {
					persisted_transaction_active = gpu_persisted_prepared;
                    std::vector<gpu_admitted_owner>
                        gpu_query_admitted_owners;
                    gpu_query_admitted_owners.reserve(batch_size);
                    std::array<std::optional<gpu_row_view>,
                        dispatch_batch_capacity> gpu_query_recipe_views{};
                    std::array<std::optional<gpu_row_view>,
                        dispatch_batch_capacity> gpu_target_recipe_views{};
                    std::array<std::optional<std::size_t>,
                        dispatch_batch_capacity> gpu_query_resident{};
                    bool any_source_view = false;
                    if (scanner_pg_ctx != nullptr) {
                        for (std::size_t row = 0u;
                             row < batch_size && !any_source_view; ++row) {
                            any_source_view = scanner_pg_ctx->
                                    query_recipe_datums[row] != Datum{0}
                                || scanner_pg_ctx->
                                    target_recipe_datums[row] != Datum{0}
                                || scanner_pg_ctx->query_locators[row].has_value()
                                || scanner_pg_ctx->target_locators[row].has_value();
                        }
                    }
                    if (any_source_view) {
                        if (scanner_pg_ctx == nullptr
                            || scanner_pg_ctx->parent_mctx == nullptr
                            || scanner_pg_ctx->read_session == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "Vulkan recipe batch has no read-session arena");
                        }
                        auto source_view_for_row =
                            [scanner_pg_ctx, alphabet_code,
                             storage_unit_byte_width](const std::size_t row,
                                const bool query_operand)
                                -> std::optional<gpu_row_view> {
                            const Datum recipe = query_operand
                                ? scanner_pg_ctx->query_recipe_datums[row]
                                : scanner_pg_ctx->target_recipe_datums[row];
                            const auto& locator = query_operand
                                ? scanner_pg_ctx->query_locators[row]
                                : scanner_pg_ctx->target_locators[row];
                            if (recipe != Datum{0} && locator.has_value()) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                    "Vulkan source operand overlaps recipe and locator ownership");
                            }
                            Datum source_datum = recipe;
                            pg_sequence_descriptor descriptor = query_operand
                                ? scanner_pg_ctx->query_recipe_descriptors[row]
                                : scanner_pg_ctx->target_recipe_descriptors[row];
                            const std::uint8_t source_alphabet = query_operand
                                ? scanner_pg_ctx->query_alphabet_code
                                : alphabet_code;
                            const std::uint8_t source_width = query_operand
                                ? scanner_pg_ctx->query_storage_unit_byte_width
                                : storage_unit_byte_width;
                            if (locator.has_value()) {
                                const std::size_t letters_per_unit =
                                    bioseq::storage::packed_letters_per_unit(
                                        source_width,
                                        static_cast<std::uint8_t>(
                                            p_sequence::letter_bit_width));
                                if (letters_per_unit == 0u) {
                                    throw bioseq::gpu::vulkan::
                                        vulkan_runtime_error(
                                            "Vulkan locator has invalid packed geometry");
                                }
                                const std::uint64_t remainder =
                                    locator->logical_length % letters_per_unit;
                                const std::uint64_t padding = remainder == 0u
                                    ? 0u : letters_per_unit - remainder;
                                if (padding
                                    > (std::numeric_limits<std::uint16_t>::max)()) {
                                    throw bioseq::gpu::vulkan::
                                        vulkan_runtime_error(
                                            "Vulkan locator padding exceeds ABI");
                                }
                                bioseq::pg_types::seq* stub =
                                    bioseq::postgres::support::invoke_pg_as_cpp(
                                        [scanner_pg_ctx, source_alphabet,
                                         source_width, padding,
                                         source_locator = *locator] {
                                            return bioseq::storage::
                                                alloc_locator_stub_in(
                                                    scanner_pg_ctx->parent_mctx,
                                                    source_alphabet,
                                                    source_width,
                                                    static_cast<std::uint16_t>(
                                                        padding),
                                                    source_locator.logical_length,
                                                    source_locator);
                                        });
                                source_datum = bioseq::storage::
                                    promote_pg_sequence_locator(
                                        scanner_pg_ctx->source_provider,
                                        static_cast<AttrNumber>(query_operand
                                            ? scanner_pg_ctx->mmsearch_query_attno
                                            : scanner_pg_ctx->mmsearch_seq_attno),
                                        PointerGetDatum(stub),
                                        scanner_pg_ctx->parent_mctx);
                                descriptor =
                                    bioseq::postgres::support::invoke_pg_as_cpp(
                                        [source_datum] {
                                            return ::
                                                classify_and_describe_sequence(
                                                    source_datum);
                                        });
                                if (descriptor.bp_length
                                        != locator->logical_length) {
                                    throw bioseq::gpu::vulkan::
                                        vulkan_runtime_error(
                                            "Vulkan locator promotion changed sequence length");
                                }
                            }
                            if (source_datum == Datum{0}) {
                                return std::nullopt;
                            }
                            const bool expanded = descriptor.kind
                                    == pg_sequence_datum_kind::expanded_read_only
                                || descriptor.kind
                                    == pg_sequence_datum_kind::expanded_read_write;
                            if (!expanded || !descriptor.alphabet_bound
                                || descriptor.alphabet_code != source_alphabet
                                || descriptor.storage_unit_byte_width
                                    != source_width) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                    "Vulkan source facade metadata is invalid");
                            }
                            auto* owner =
                                bioseq::postgres::support::invoke_pg_as_cpp(
                                    [scanner_pg_ctx, source_datum,
                                     descriptor] {
                                        return ::make_pg_sequence_call_owner<
                                            gpu_row_alphabet>(
                                                *scanner_pg_ctx->read_session,
                                                source_datum, descriptor);
                                    });
                            const auto* observer = query_operand
                                ? std::addressof(scanner_pg_ctx->
                                    mmsearch_query_payload_observer)
                                : std::addressof(scanner_pg_ctx->
                                    mmsearch_target_payload_observer);
                            return owner->view().
                                with_payload_allocation_observer(observer);
                        };
                        for (std::size_t row = 0u; row < batch_size; ++row) {
                            const auto query_source = source_view_for_row(
                                row, /*query_operand=*/true);
                            if (query_source.has_value()) {
                                const gpu_row_view original = *query_source;
                                const auto access =
                                    bioseq::storage::plan_sequence_access(
                                        original,
                                        bioseq::storage::
                                            sequence_access_requirement::
                                                contiguous_resident,
                                        0u, original.bp_size(),
                                        storage_unit_byte_width);
                                const std::uint64_t admitted_bytes =
                                    access.route == bioseq::storage::
                                            sequence_access_route::
                                                materialize_admitted
                                        ? access.admission_bytes
                                        : bioseq::storage::
                                            packed_sequence_admission_bytes<
                                                gpu_row_alphabet>(
                                                    original.bp_size(),
                                                    storage_unit_byte_width);
                                auto permit = bioseq::parallelism::
                                    memory_admission_controller::instance().
                                        try_reserve(admitted_bytes);
                                if (!permit.has_value()) {
                                    throw bioseq::gpu::vulkan::
                                        vulkan_runtime_error(
                                            "Vulkan query recipe packed-region admission was rejected");
                                }
                                gpu_query_admitted_owners.push_back(
                                    bioseq::storage::materialize_packed_sequence(
                                        original, 0u, original.bp_size(),
                                        storage_unit_byte_width,
                                        std::move(*permit)));
                                gpu_query_resident[row] =
                                    gpu_query_admitted_owners.size() - 1u;
                                gpu_query_recipe_views[row] =
                                    gpu_query_admitted_owners.back().view();
                            }
                            const auto target_source = source_view_for_row(
                                row, /*query_operand=*/false);
                            if (target_source.has_value()) {
                                gpu_target_recipe_views[row] = *target_source;
                            }
                        }
                    }
                    const std::size_t w =
                        static_cast<std::size_t>(dispatch_params.window_size);
                    const std::size_t k =
                        static_cast<std::size_t>(dispatch_params.kmer_size);
                    // The resident query sketch remains device-only.  A fair
                    // no-index target build is complete and query-independent;
                    // host-materialising query hashes to filter that build
                    // would create a direct-match accelerator, not an index.
                    std::shared_ptr<bioseq::gpu::vulkan::
                        vulkan_batch_sketch_memory_retention>
                        query_sketch_memory_retention;
                    std::vector<bioseq::gpu::vulkan::gpu_seed_query_record_wire>
                        query_records;
                    bioseq::gpu::vulkan::vulkan_device_minimizer_span<minimizer_type>
                        query_minimizer_device_span{};
                    bioseq::gpu::vulkan::device_query_sketch_authorization
                        query_sketch_authority{};
                    std::vector<gpu_batch_query_sidecar> query_sidecars;
                    std::vector<std::uint32_t> row_to_query_index(
                        batch_size,
                        std::numeric_limits<std::uint32_t>::max());
                    pipe::ordered_sketch_query_gate_storage query_gate;
                    std::vector<bioseq::gpu::vulkan::sequence_upload_view>
                        batch_query_uploads;
                    const auto gpu_batch_now_ns = []() -> std::uint64_t {
                        return static_cast<std::uint64_t>(
                            bioseq::instrumentation::clock::now_ns());
                    };
                    const std::uint64_t gpu_query_sketch_begin_ns =
                        gpu_batch_now_ns();

                    query_sidecars.reserve(batch_size);
                    batch_query_uploads.reserve(batch_size);
                    for (std::size_t i = 0; i < batch_size; ++i) {
                        const bioseq::pg_types::seq* row_query_hdr =
                            per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                        const bool recipe_query =
                            gpu_query_resident[i].has_value();
                        if (row_query_hdr != nullptr && recipe_query) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "Vulkan query recipe overlaps a flat query");
                        }
                        if (row_query_hdr == nullptr && !recipe_query) {
                            continue;
                        }
                        if (row_query_hdr != nullptr
                            && (row_query_hdr->alphabet_code != alphabet_code
                            || row_query_hdr->storage_unit_byte_width
                                != storage_unit_byte_width)) {
                            continue;
                        }
                        const std::uint32_t query_index =
                            static_cast<std::uint32_t>(batch_query_uploads.size());
                        sequence_size_t query_length = 0u;
                        if (recipe_query) {
                            const auto owner_index =
                                *gpu_query_resident[i];
                            if (owner_index
                                    >= gpu_query_admitted_owners.size()) {
                                throw bioseq::gpu::vulkan::
                                    vulkan_runtime_error(
                                        "Vulkan query recipe owner index is invalid");
                            }
                            const auto& admitted =
                                gpu_query_admitted_owners[owner_index];
                            batch_query_uploads.push_back(
                                bioseq::gpu::vulkan::
                                    make_sequence_upload_view<
                                        gpu_row_alphabet>(admitted));
                            query_length = admitted.descriptor().logical_length;
                        } else {
                            compact_sequence_range<p_sequence> query_seq{
                                *row_query_hdr};
                            batch_query_uploads.push_back(
                                bioseq::gpu::vulkan::
                                    make_sequence_upload_view(query_seq));
                            query_length = query_seq.bp_size();
                        }
                        query_sidecars.push_back(gpu_batch_query_sidecar{
                            .row_index = i,
                            .query_len = query_length,
                        });
                        row_to_query_index[i] = query_index;
                    }
                    if (!batch_query_uploads.empty()) {
                        const auto rank_lut =
                            bioseq::gpu::vulkan::make_vulkan_rank_lut<
                                compact_sequence_range<p_sequence>>();
                        auto gpu_batch_sketch =
                            bioseq::gpu::vulkan::
                                collect_minimap_sketch_data_vulkan_upload_batch_result(
                                    std::span<
                                        const bioseq::gpu::vulkan::sequence_upload_view>{
                                        batch_query_uploads.data(),
                                        batch_query_uploads.size()},
                                    rank_lut,
                                    w,
                                    k,
                                    bioseq::gpu::vulkan::
                                        vulkan_sketch_host_materialization::
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
					 * authenticated PostgreSQL-owned grouped-SoA buffers.  Consume that
					 * representation directly: do not materialize the target sequence,
					 * fingerprint it, rebuild an alternate slot table, or read target symbols
					 * on the host. */
					auto* const prepared_gpu_target = scanner_pg_ctx != nullptr
						? scanner_pg_ctx->gpu_prepared_grouped_soa_target : nullptr;
					if (prepared_gpu_target != nullptr) {
						if (!gpu_persisted_prepared
							|| !prepared_gpu_target->ready()
							|| prepared_gpu_target->resident == nullptr
							|| query_records.size() != query_sidecars.size()
							|| prepared_gpu_target->target_bp_size
								> static_cast<std::uint64_t>(
									(std::numeric_limits<sequence_size_t>::max)())) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA prepared-target identity is invalid");
						}
						const auto requested_source =
							requested_target_index_source(scanner_pg_ctx);
						if (requested_source
								!= ::bioseq::pipeline_stats::target_index_source::persisted
							|| dispatch_params.max_occ < 0
							|| static_cast<std::uint64_t>(dispatch_params.max_occ)
								> prepared_gpu_target->resident_view.max_occ_cap) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA source or max_occ contract is invalid");
						}

						/* Atomic publication keeps the direct inputs, runtime results
						 * and staged SQL values alive together. Admit that transient overlap
						 * before the first covered N-job reserve. */
						const std::uint64_t publication_staging_bytes =
							persisted_grouped_soa_publication_staging_bytes(
								static_cast<std::uint64_t>(query_sidecars.size()));
						if (publication_staging_bytes
								== (std::numeric_limits<std::uint64_t>::max)()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA publication staging admission overflow");
						}
						auto publication_staging_permit =
							bioseq::parallelism::memory_admission_controller::instance()
								.try_reserve(publication_staging_bytes);
						if (!publication_staging_permit.has_value()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA publication staging admission was rejected");
						}

						using staged_job_type = ::bioseq::pipeline_stats::
							pipeline_job_execution_stats;
						std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job>
							direct_jobs;
						std::vector<std::size_t> direct_job_rows;
						direct_jobs.reserve(query_sidecars.size());
						direct_job_rows.reserve(query_sidecars.size());
						for (const gpu_batch_query_sidecar& query : query_sidecars) {
							const std::size_t row = query.row_index;
							const std::uint32_t query_index =
								row_to_query_index[row];
							if (query_index
									== std::numeric_limits<std::uint32_t>::max()
								|| query_index >= query_records.size()
								|| scanner_pg_ctx->target_rownos[row]
									!= prepared_gpu_target->target_rowno) {
								throw bioseq::gpu::vulkan::vulkan_runtime_error(
									"persisted GPU grouped-SoA query/target row identity is inconsistent");
							}
							direct_jobs.push_back(
								bioseq::gpu::vulkan::gpu_seed_coordinate_job{
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
									.grouped_soa =
										&prepared_gpu_target->resident_view,
								});
							direct_job_rows.push_back(row);
						}
						if (direct_jobs.empty()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA batch has no admissible GPU query job");
						}
						if (query_minimizer_device_span.count()
								> static_cast<std::uint64_t>(
									(std::numeric_limits<std::uint32_t>::max)())) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA query minimizer count exceeds ABI");
						}

						bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
							.host_minimizer_bytes = {},
							.device_minimizer_words =
								query_minimizer_device_span.words(),
							.minimizer_count = static_cast<std::uint32_t>(
								query_minimizer_device_span.count()),
							.minimizer_word_kind = static_cast<std::uint32_t>(
								bioseq::gpu::vulkan::minimizer_wire_traits<
									minimizer_type>::k_word_kind),
							.minimizer_words32_per_record =
								bioseq::gpu::vulkan::minimizer_wire_traits<
									minimizer_type>::k_words32_per_minimizer,
							.hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<
								minimizer_type>::k_hash_shift,
							.pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<
								minimizer_type>::k_position_bits,
							.window_size = static_cast<std::uint32_t>(
								dispatch_params.window_size),
							.host_query_records = std::span<const
								bioseq::gpu::vulkan::gpu_seed_query_record_wire>{
								query_records.data(), query_records.size()},
							.device_query_records = nullptr,
						};
						auto cp = dispatch_params.make_chaining_parameters();
						const std::int32_t max_dist_x =
							std::max(cp.max_gap, cp.bandwidth);
						const std::int32_t max_dist_y =
							std::max(cp.max_gap, cp.bandwidth);
						const auto chain_key =
							bioseq::chaining::choose_vulkan_chaining_pipeline(
								runtime.features());
						constexpr auto coordinate_algorithm = bioseq::gpu::vulkan::
							gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
						const std::int32_t max_iter =
							std::min<std::int32_t>(cp.max_chain_iter, 3000);
						constexpr std::uint32_t probe_limit = 16u;
						const auto coordinate_configuration =
							bioseq::gpu::vulkan::
								make_persisted_grouped_soa_coordinate_config_identity(
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
							run_seed_to_coordinate_batch_roundtrip(
								query_inputs,
								std::span<const bioseq::gpu::vulkan::
									gpu_seed_coordinate_job>{
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

						const auto& direct = coord_run.grouped_soa_direct;
						const bool direct_validation_valid = direct.selected
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
							&& coord_run.operation_receipt.is_sealed()
							&& coord_run.operation_receipt.job_count()
								== direct_jobs.size();
						if (!direct_validation_valid) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA direct-route stats are incomplete or inconsistent");
						}

						::bioseq::pipeline_stats::pipeline_execution_stats
							staged_stats{};
						add_stage_host_time(
							staged_stats,
							::bioseq::pipeline_stats::pipeline_stage::query_sketch,
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

						std::vector<staged_job_type> staged_jobs;
						staged_jobs.reserve(direct_jobs.size());
						decltype(results) staged_results;
						std::array<bool, dispatch_batch_capacity> staged_rows{};

						for (std::size_t problem_index = 0;
							 problem_index < direct_jobs.size();
							 ++problem_index) {
							const std::size_t row = direct_job_rows[problem_index];
							if (row >= batch_size || row >= dispatch_batch_capacity
								|| staged_rows[row] || query_ids[row] < 0
								|| direct_jobs[problem_index].logical_target_row
									> ::bioseq::pipeline_stats::k_sql_bigint_value_max
								|| direct_jobs[problem_index].logical_query_ordinal
									> ::bioseq::pipeline_stats::k_sql_bigint_value_max
								|| prepared_gpu_target->resident_view.generation_id
									> ::bioseq::pipeline_stats::k_sql_bigint_value_max
								|| scanner_pg_ctx->target_index_sources[row]
									!= ::bioseq::pipeline_stats::target_index_source::none
								|| scanner_pg_ctx->persisted_generation_ids[row] != 0u
								|| scanner_pg_ctx->persisted_source_identities[row] != 0u) {
								throw bioseq::gpu::vulkan::vulkan_runtime_error(
									"persisted GPU grouped-SoA staged row publication is invalid");
							}
							staged_rows[row] = true;
							const auto& coord = coord_run.results[problem_index];
							const auto& query_record =
								query_records[direct_jobs[problem_index].query_index];
							auto staged_job = stage_persisted_grouped_soa_coordinate_job(
								dispatch_params,
								requested_source,
								coord_run.operation_receipt,
								bioseq::gpu::vulkan::
									persisted_grouped_soa_coordinate_job_expectation{
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
										.abi_revision = prepared_gpu_target
											->resident_view.abi_revision,
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
							if (coord.is_valid != 0u) {
								const std::uint32_t query_index =
									direct_jobs[problem_index].query_index;
								staged_results[query_ids[row]].push_back(
									pipe::gpu_coordinate_to_match_result(
										coord,
										query_sidecars[query_index].query_len,
										static_cast<sequence_size_t>(
											prepared_gpu_target->target_bp_size)));
							}
						}
						const std::uint64_t expected_jobs =
							static_cast<std::uint64_t>(direct_jobs.size());
						const std::uint64_t validated_jobs = static_cast<std::uint64_t>(
							coord_run.operation_receipt.job_count());
						const std::uint64_t consumed_jobs =
							static_cast<std::uint64_t>(
								coord_run.operation_receipt.consumed_count());
						const std::uint64_t pending_jobs = validated_jobs >= consumed_jobs
							? validated_jobs - consumed_jobs
							: (std::numeric_limits<std::uint64_t>::max)();
						const ::bioseq::pipeline_stats::
							persisted_grouped_soa_coordinate_publication_stats
							publication_counts{
								.expected_jobs = expected_jobs,
								.validated_jobs = validated_jobs,
								.consumed_jobs = consumed_jobs,
								.accepted_jobs = static_cast<std::uint64_t>(
									coord_run.operation_receipt.accepted_count()),
								.burned_jobs = static_cast<std::uint64_t>(
									coord_run.operation_receipt.burned_count()),
								.pending_jobs = pending_jobs,
								.staged_jobs = static_cast<std::uint64_t>(
									staged_jobs.size()),
							};
						if (!::bioseq::pipeline_stats::
								is_persisted_grouped_soa_coordinate_publication_consistent(
									publication_counts)
							|| !coord_run.operation_receipt.are_all_jobs_accepted()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA operation stats were not accepted exactly once");
						}
						const bool lifecycle_prestate_valid =
							prepared_gpu_target->query_count != 0u
							&& prepared_gpu_target->query_count
								<= ::bioseq::pipeline_stats::k_sql_bigint_value_max
							&& prepared_gpu_target->dispatched_query_count
								<= prepared_gpu_target->query_count
							&& (prepared_gpu_target->dispatched_query_count == 0u
								? prepared_gpu_target->dispatch_waves == 0u
								: prepared_gpu_target->dispatch_waves != 0u)
							&& expected_jobs <= prepared_gpu_target->query_count
								- prepared_gpu_target->dispatched_query_count;
						if (!lifecycle_prestate_valid
							|| prepared_gpu_target->dispatched_query_count
								> ::bioseq::pipeline_stats::k_sql_bigint_value_max
									- expected_jobs
							|| prepared_gpu_target->dispatch_waves
								>= ::bioseq::pipeline_stats::k_sql_bigint_value_max) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA lifecycle counters overflow");
						}

						if (!results.empty()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA result staging did not start empty");
						}
						const std::size_t effective_workers =
							bioseq::parallelism::global_pool::instance().maximum_concurrency();
						if (effective_workers == 0u
							|| effective_workers
								> ::bioseq::pipeline_stats::k_sql_integer_value_max) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA CPU worker identity is invalid");
						}
						const auto cache_metrics_preview =
							coord_run.operation_receipt.cache_metrics_preview();
						const auto environment_preview =
							coord_run.operation_receipt.environment_preview();
						const auto staged_environment = ::bioseq::pipeline_stats::
							make_persisted_grouped_soa_gpu_environment_snapshot(
								environment_preview, effective_workers);
						const auto maximum_cache_metrics = ::bioseq::pipeline_stats::
							make_persisted_grouped_soa_gpu_cache_metrics_delta(
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
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA environment/cache preview is invalid");
						}
						auto complete_environment = *staged_environment;
						complete_environment.record_highway_target(
							dispatch_params.highway_dispatch,
							dispatch_params.highway_executed_target);
						if (!::bioseq::pipeline_stats::is_environment_valid_for_backend(
								complete_environment,
								::bioseq::pipeline_stats::executed_backend::vulkan_gpu)) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA Highway environment stats are invalid");
						}
						for (staged_job_type& job : staged_jobs) {
							job.environment = complete_environment;
							staged_stats.merge_job(job);
						}
						if (record_target_operational_outcomes) {
							record_target_row(staged_stats);
							record_target_index_outcome(
								staged_stats,
								requested_source,
								::bioseq::pipeline_stats::target_index_source::persisted);
						}
						auto maximum_staged_stats = staged_stats;
						maximum_staged_stats.merge_operational(*maximum_cache_metrics);
						auto maximum_prospective_stats = stats;
						maximum_prospective_stats.merge(maximum_staged_stats);
						if (!::bioseq::pipeline_stats::
								is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
									stats,
									maximum_staged_stats,
									maximum_prospective_stats,
									expected_jobs)) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA prospective stats are invalid");
						}

						/* SQL stats use a separate admitted statement-root ledger. Build,
						 * validate and preflight its complete operation image now: nothing may
						 * allocate, search for duplicates or decide capacity after Runtime's
						 * final immutable gate claims report-once cache authority. */
						const auto operation_receipt_preview =
							coord_run.operation_receipt.operation_receipt_preview();
						auto operation_stats_chunk = ::bioseq::pipeline_stats::
							persisted_gpu_operation_chunk::prepare(
								direct_jobs.size());
						if (!operation_receipt_preview.is_internally_consistent()
							|| operation_stats_chunk == nullptr
							|| scanner_pg_ctx->persisted_gpu_operation_stats_sink
								== nullptr
							|| operation_receipt_preview.device_uuid
								!= prepared_gpu_target->resident_view.device_uuid
							|| operation_receipt_preview.acceptance.operation_identity
								!= cache_metrics_preview.token_identity) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA SQL operation stats are unavailable");
						}
						operation_stats_chunk->operation() =
							make_persisted_gpu_operation_stats(
								operation_receipt_preview,
								publication_counts,
								prepared_gpu_target->resident_view.generation_id,
								prepared_gpu_target->resident_view.source_identity,
								prepared_gpu_target->resident_view.cold_load_identity);
						for (std::size_t problem_index = 0u;
							 problem_index < direct_jobs.size(); ++problem_index) {
							const auto job_stats_preview = coord_run.operation_receipt.
								job_evidence_preview(problem_index);
							if (!job_stats_preview.has_value()
								|| job_stats_preview->audit.operation_identity
									!= operation_receipt_preview.acceptance
										.operation_identity
								|| job_stats_preview->audit.generation_id
									!= prepared_gpu_target->resident_view.generation_id
								|| job_stats_preview->audit.source_identity
									!= prepared_gpu_target->resident_view.source_identity
								|| job_stats_preview->audit.cold_load_identity
									!= prepared_gpu_target->resident_view.cold_load_identity
								|| job_stats_preview->audit.logical_target_row
									!= direct_jobs[problem_index].logical_target_row
								|| job_stats_preview->audit.logical_query_ordinal
									!= direct_jobs[problem_index].logical_query_ordinal
								|| job_stats_preview->audit.query_index
									!= direct_jobs[problem_index].query_index) {
								throw bioseq::gpu::vulkan::vulkan_runtime_error(
									"persisted GPU grouped-SoA SQL job stats are inconsistent");
							}
							operation_stats_chunk->jobs()[problem_index] =
								make_persisted_gpu_job_stats(*job_stats_preview);
						}
						if (!operation_stats_chunk->seal()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA SQL operation stats did not seal");
						}
						auto operation_stats_append =
							scanner_pg_ctx->persisted_gpu_operation_stats_sink
								->prepare_append(operation_stats_chunk);
						if (!operation_stats_append.is_valid()) {
							throw bioseq::gpu::vulkan::vulkan_runtime_error(
								"persisted GPU grouped-SoA SQL stats ledger rejected the operation");
						}

						/* Every allocation, row check and overflow check is complete.
						 * The runtime finalizer validates the causal receipt union and
						 * performs the final immutable-policy live gate before minting
						 * this move-only authority. */
						auto accepted_operation = runtime.
							finalize_persisted_grouped_soa_coordinate_operation(
								std::move(coord_run.operation_receipt),
								operation_receipt_preview);
						const auto& acceptance = accepted_operation.audit();
						const auto& authoritative_environment =
							accepted_operation.environment_binding();
						const auto authoritative_cache_metrics =
							accepted_operation.cache_metrics_audit();
						::bioseq::pipeline_stats::pipeline_operational_stats
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
							::bioseq::pipeline_stats::pipeline_execution_stats>);
						static_assert(std::is_nothrow_copy_assignable_v<
							::bioseq::pipeline_stats::pipeline_execution_stats>);
						static_assert(noexcept(
							committed_staged_stats.merge_operational(
								authoritative_cache_delta)));
						static_assert(noexcept(
							prospective_stats.merge(committed_staged_stats)));
						static_assert(std::is_nothrow_move_constructible_v<
							decltype(results)>);
						static_assert(noexcept(results.swap(staged_results)));
						static_assert(noexcept(::bioseq::pipeline_stats::
							is_persisted_grouped_soa_gpu_environment_consistent(
								authoritative_environment, environment_preview)));
						static_assert(noexcept(::bioseq::pipeline_stats::
							is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
								authoritative_cache_metrics, cache_metrics_preview)));
						static_assert(noexcept(cache_metrics_delta.consume()));
						static_assert(noexcept(
							operation_stats_chunk->publish_cache_metrics(
								make_persisted_gpu_cache_metrics_stats(
									authoritative_cache_metrics))));
						static_assert(noexcept(
							scanner_pg_ctx->persisted_gpu_operation_stats_sink
								->commit_append(
									std::move(operation_stats_append),
									std::move(operation_stats_chunk))));
						/* The finalizer compared this same preview before its live gate
						 * and report-once claim. These are construction assertions, not a
						 * recoverable post-finalizer rejection path. */
						DEBUG_ASSERT(accepted_operation.is_valid(),
							"persisted GPU accepted operation authority vanished");
						DEBUG_ASSERT(acceptance == operation_receipt_preview.acceptance,
							"persisted GPU operation changed after finalizer");
						DEBUG_ASSERT(acceptance.expected_job_count == expected_jobs,
							"persisted GPU job count changed after finalizer");
						DEBUG_ASSERT(::bioseq::pipeline_stats::
							is_persisted_grouped_soa_gpu_environment_consistent(
								authoritative_environment, environment_preview),
							"persisted GPU environment changed after finalizer");
						DEBUG_ASSERT(::bioseq::pipeline_stats::
							is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
								authoritative_cache_metrics, cache_metrics_preview),
							"persisted GPU cache metrics exceeded preview");
						DEBUG_ASSERT(cache_metrics_delta.is_valid()
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
							&operation_stats_chunk,
							&operation_stats_append]() noexcept {
							operation_stats_chunk->publish_cache_metrics(
								make_persisted_gpu_cache_metrics_stats(
									authoritative_cache_metrics));
							/* accepted() plus the exact audit equality above are consume()'s
							 * complete precondition. Burn the one-shot authority now; no
							 * rejecting edge may follow before SQL-visible publication. */
							cache_metrics_delta.consume();
							results.swap(staged_results);
							stats = prospective_stats;
							for (const std::size_t row : direct_job_rows) {
								scanner_pg_ctx->target_index_sources[row] =
									::bioseq::pipeline_stats::target_index_source::persisted;
								scanner_pg_ctx->persisted_generation_ids[row] =
									prepared_gpu_target->resident_view.generation_id;
								scanner_pg_ctx->persisted_source_identities[row] =
									prepared_gpu_target->resident_view.source_identity;
							}
							prepared_gpu_target->dispatched_query_count += expected_jobs;
							++prepared_gpu_target->dispatch_waves;
							scanner_pg_ctx->persisted_gpu_operation_stats_sink
								->commit_append(
									std::move(operation_stats_append),
									std::move(operation_stats_chunk));
						};
						static_assert(noexcept(commit_publication()));
						commit_publication();
						return results;
					}
					add_stage_host_time(
						stats,
						::bioseq::pipeline_stats::pipeline_stage::query_sketch,
						gpu_query_sketch_host_ns);

                    std::vector<gpu_batch_target_sidecar> targets;
                    std::vector<std::uint32_t> row_to_target_index(
                        batch_size,
                        std::numeric_limits<std::uint32_t>::max());
                    targets.reserve(batch_size);
                    const auto target_storage_bytes =
                        [] (const bioseq::pg_types::seq* hdr) -> std::string_view {
                        compact_sequence_range<p_sequence> seq{*hdr};
                        using storage_unit_type =
                            typename compact_sequence_range<p_sequence>::traits::storage_unit;
                        const auto* bytes =
                            reinterpret_cast<const char*>(seq.cbegin());
                        return std::string_view{
                            bytes,
                            seq.size() * sizeof(storage_unit_type)};
                    };
                    const auto target_fingerprint =
                        [target_storage_bytes] (
                            const bioseq::pg_types::seq* hdr) -> std::uint64_t {
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
	                        const bioseq::pg_types::seq* target_hdr = target_hdrs[i];
	                        const bool recipe_target =
	                            gpu_target_recipe_views[i].has_value();
	                        const Datum target_recipe_datum = recipe_target
	                            ? scanner_pg_ctx->target_recipe_datums[i]
	                            : Datum{0};
	                        if (target_hdr != nullptr && recipe_target) {
	                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
	                                "Vulkan target recipe overlaps a flat target");
	                        }
	                        if (target_hdr == nullptr && !recipe_target) {
	                            if (scanner_pg_ctx != nullptr
	                                && scanner_pg_ctx->target_locators[i].has_value()) {
	                                const auto& loc =
	                                    *scanner_pg_ctx->target_locators[i];
	                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
	                                    fmt::format(
	                                        "selected Vulkan batch target row {} "
	                                        "is only available as a PG locator "
	                                        "({} bp); selected Vulkan requires a "
	                                        "materialized target payload and "
	                                        "cannot use scanner_pg/MaxAllocSize "
	                                        "streaming",
	                                        static_cast<int>(query_ids[i]),
	                                        static_cast<unsigned long long>(
	                                            loc.logical_length)));
	                            }
	                            continue;
	                        }
	                        if (target_hdr != nullptr
	                            && (target_hdr->alphabet_code != alphabet_code
	                            || target_hdr->storage_unit_byte_width
	                                != storage_unit_byte_width)) {
	                            continue;
	                        }
	                        std::uint32_t target_index =
	                            std::numeric_limits<std::uint32_t>::max();
	                        for (std::size_t existing = 0;
	                             existing < targets.size();
	                             ++existing) {
	                            const bool same_source_recipe = recipe_target
	                                && target_recipe_datum != Datum{0}
	                                && targets[existing].recipe_datum
	                                    == target_recipe_datum;
	                            const bool same_source_locator = recipe_target
	                                && target_recipe_datum == Datum{0}
	                                && targets[existing].recipe_view.has_value()
	                                && scanner_pg_ctx->target_locators[i].has_value()
	                                && scanner_pg_ctx->target_locators[
	                                    targets[existing].source_row_index].has_value()
	                                && same_dispatch_target_locator(
	                                    *scanner_pg_ctx->target_locators[i],
	                                    *scanner_pg_ctx->target_locators[
	                                        targets[existing].source_row_index]);
	                            if ((!recipe_target
	                                    && targets[existing].hdr == target_hdr)
	                                || same_source_recipe
	                                || same_source_locator) {
	                                target_index = static_cast<std::uint32_t>(existing);
	                                break;
	                            }
	                        }
	                        std::uint64_t fingerprint = 0u;
	                        if (target_index
	                                == std::numeric_limits<std::uint32_t>::max()
	                            && !recipe_target) {
	                            fingerprint = target_fingerprint(target_hdr);
	                        }
	                        for (std::size_t existing = 0;
	                             target_index
	                                 == std::numeric_limits<std::uint32_t>::max()
	                             && !recipe_target
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
                        const sequence_size_t target_length = recipe_target
                            ? gpu_target_recipe_views[i]->bp_size()
                            : static_cast<sequence_size_t>(
                                target_hdr->bp_size);
                        if (target_index
                            == std::numeric_limits<std::uint32_t>::max()) {
                            target_index =
                                static_cast<std::uint32_t>(targets.size());
                            targets.push_back(gpu_batch_target_sidecar{
                                .hdr = target_hdr,
                                .recipe_view = recipe_target
                                    ? gpu_target_recipe_views[i]
                                    : std::nullopt,
                                .recipe_datum = target_recipe_datum,
                                .source_row_index = i,
                                .fingerprint = fingerprint,
                                .target_len = target_length,
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
                         &cached_query_minimizer_stats_accounted,
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
                                ::bioseq::pipeline_stats::target_index_source::recomputed,
                                scanner_pg_ctx,
                                row,
                                per_row_query_mode,
                                broadcast_query_ordinal,
                                /*execution_stats=*/nullptr,
                                /*consumed_shared_cached_query_stats=*/false,
                                cached_query_minimizer_stats_accounted);
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
                                ::bioseq::pipeline_stats::target_index_source::recomputed,
                                unique_target_count);
                        }
                    };
                    if (query_records.empty() || targets.empty()) {
                        record_gpu_batch_outcomes();
                        return results;
                    }

                    const auto frozen_query_gate =
                        pipe::freeze_gpu_query_gate(
                            query_gate.vulkan_gate());
                    const auto frozen_gate_identity =
                        frozen_query_gate.canonical_identity();

                    const std::size_t lpu = facade_letters_per_unit;
                    for (std::size_t target_index = 0;
                         target_index < targets.size();
                        ++target_index) {
                        gpu_batch_target_sidecar& target = targets[target_index];
                        std::optional<compact_sequence_range<p_sequence>>
                            flat_target_sequence;
                        const gpu_row_view target_view =
                            [&target, &flat_target_sequence] {
                            if (target.recipe_view.has_value()) {
                                return *target.recipe_view;
                            }
                            if (target.hdr == nullptr) {
                                throw bioseq::gpu::vulkan::
                                    vulkan_runtime_error(
                                        "Vulkan target sidecar has no backing view");
                            }
                            flat_target_sequence.emplace(*target.hdr);
                            return bioseq::storage::make_lazy_sequence_view(
                                *flat_target_sequence);
                        }();
                        const std::uint64_t gpu_target_plan_begin_ns =
                            gpu_batch_now_ns();
                        constexpr sequence_size_t gpu_batch_huge_target_bp =
                            100'000'000ull;
                        constexpr sequence_size_t gpu_batch_huge_target_core_bp =
                            8'000'000ull;
                        constexpr sequence_size_t gpu_batch_huge_target_overlap_max_bp =
                            30'000'000ull;
                        if (target.row_count > 1
                            && target.target_len >= gpu_batch_huge_target_bp) {
                            target.ranges.clear();
                            const sequence_size_t total_units =
                                (target.target_len + lpu - 1u) / lpu;
                            sequence_size_t overlap_bp =
                                target.max_query_len
                                + target.max_query_len / 4u
                                + 100'000ull;
                            overlap_bp = std::min(
                                overlap_bp,
                                gpu_batch_huge_target_overlap_max_bp);
                            const sequence_size_t overlap_units =
                                (overlap_bp + lpu - 1u) / lpu;
                            const sequence_size_t segment_units =
                                std::max<sequence_size_t>(
                                    1u,
                                    gpu_batch_huge_target_core_bp / lpu);
                            for (sequence_size_t lo_u = 0; lo_u < total_units;
                                 lo_u += segment_units) {
                                const sequence_size_t hi_u =
                                    std::min(lo_u + segment_units, total_units);
                                const sequence_size_t cs_u =
                                    lo_u > overlap_units ? lo_u - overlap_units : 0u;
                                const sequence_size_t ce_u =
                                    std::min(hi_u + overlap_units, total_units);
                                target.ranges.push_back(::bioseq::indexing::segment_unit_range{
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
                                minimizer_sketch_backend::vulkan_segment);
                        }
                        add_stage_host_time(
                            stats,
                            ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                            gpu_batch_now_ns() - gpu_target_plan_begin_ns);
                        target.residents.reserve(target.ranges.size());
                        for (std::size_t segment_index = 0;
                             segment_index < target.ranges.size();
                             ++segment_index) {
                            const ::bioseq::indexing::segment_unit_range& range =
                                target.ranges[segment_index];
                            const auto [segment_lo_bp, segment_hi_bp] =
                                pipe::validated_segment_bp_bounds(
                                    range, lpu, target.target_len);
                            const std::uint64_t gpu_index_build_begin_ns =
                                gpu_batch_now_ns();
                            const bool full_target_range =
                                segment_lo_bp == 0u
                                && segment_hi_bp == target.target_len;
                            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                            auto built = full_target_range
                                ? bioseq::gpu::vulkan::build_gpu_index_on_device<
                                    minimizer_type>(
                                    target_view,
                                    w,
                                    k,
                                    frozen_query_gate,
                                    &decline)
                                : bioseq::gpu::vulkan::build_gpu_segment_index_on_device<
                                    minimizer_type>(
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
                                    ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                    gpu_batch_now_ns()
                                        - gpu_index_build_begin_ns);
                                target.residents.push_back(nullptr);
                                continue;
                            }
                            bioseq::indexing::gpu_minimizer_index_cache_key key{
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
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
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
                                ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                gpu_batch_now_ns() - gpu_index_build_begin_ns);
                        }
                    }

                    std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job>
                        gpu_jobs;
                    std::vector<gpu_batch_problem_sidecar> problem_sidecars;
                    const std::uint64_t gpu_problem_build_begin_ns =
                        gpu_batch_now_ns();
	                    gpu_jobs.reserve(batch_size);
	                    problem_sidecars.reserve(batch_size);
	                    for (std::size_t target_index = 0;
	                         target_index < targets.size();
	                         ++target_index) {
	                        gpu_batch_target_sidecar& target = targets[target_index];
	                        for (std::size_t segment_index = 0;
	                             segment_index < target.residents.size();
	                             ++segment_index) {
	                            const auto& resident = target.residents[segment_index];
	                            if (!resident) {
	                                continue;
	                            }
	                            const ::bioseq::indexing::segment_unit_range& range =
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
	                                        gpu_seed_coordinate_job{
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
	                                problem_sidecars.push_back(gpu_batch_problem_sidecar{
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
                        ::bioseq::pipeline_stats::pipeline_stage::seed_lookup,
                        gpu_batch_now_ns() - gpu_problem_build_begin_ns);

                    if (gpu_jobs.empty()) {
                        record_gpu_batch_outcomes();
                        return results;
                    }

                    auto cp = dispatch_params.make_chaining_parameters();
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
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "batched device query minimizer count exceeds ABI"};
                    }
                    bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
                        .host_minimizer_bytes = {},
                        .device_minimizer_words =
                            query_minimizer_device_span.words(),
                        .minimizer_count = static_cast<std::uint32_t>(
                            query_minimizer_device_span.count()),
                        .minimizer_word_kind =
                            static_cast<std::uint32_t>(
                                bioseq::gpu::vulkan::minimizer_wire_traits<
                                    minimizer_type>::k_word_kind),
                        .minimizer_words32_per_record =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                minimizer_type>::k_words32_per_minimizer,
                        .hash_shift =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                minimizer_type>::k_hash_shift,
                        .pos_bits =
                            bioseq::gpu::vulkan::minimizer_wire_traits<
                                minimizer_type>::k_position_bits,
                        .host_query_records = std::span<
                            const bioseq::gpu::vulkan::gpu_seed_query_record_wire>{
                            query_records.data(),
                            query_records.size()},
                        .device_query_records = nullptr,
                    };
                    const auto coord_run =
                        runtime.run_seed_to_coordinate_batch_roundtrip(
                            query_inputs,
                            std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{
                                gpu_jobs.data(),
                                gpu_jobs.size()},
                            static_cast<std::uint32_t>(dispatch_params.kmer_size),
                            dispatch_params.max_occ,
                            bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::
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
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
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

                    struct gpu_cigar_pending_match {
                        int32_t query_id = 0;
                        match_result match{};
                    };
                    std::vector<gpu_cigar_pending_match> pending_cigar_matches;
                    std::vector<bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire>
                        cigar_jobs;
                    std::vector<std::uint32_t> cigar_query_symbols;
                    std::vector<std::uint32_t> cigar_target_symbols;
                    std::vector<std::uint8_t> cigar_symbol_scratch;
                    std::uint32_t packed_cigar_capacity = 0;
                    std::uint32_t traceback_word_capacity = 0;
                    pending_cigar_matches.reserve(coord_run.results.size());
                    cigar_jobs.reserve(coord_run.results.size());
                    const auto gpu_cigar_now_ns = []() -> std::uint64_t {
                        return static_cast<std::uint64_t>(
                            bioseq::instrumentation::clock::now_ns());
                    };
                    const std::uint64_t cigar_prepare_begin_ns =
                        gpu_cigar_now_ns();

                    for (std::size_t problem_index = 0;
                         problem_index < coord_run.results.size();
                         ++problem_index) {
                        const auto& coord = coord_run.results[problem_index];
                        if (coord.is_valid == 0u) {
                            continue;
                        }
                        const gpu_batch_problem_sidecar& owner =
                            problem_sidecars[problem_index];
                        if (coord.target_start < owner.core_lo_bp
                            || coord.target_start >= owner.core_hi_bp) {
                            continue;
                        }
                        const std::uint32_t query_index =
                            row_to_query_index[owner.row_index];
                        const gpu_batch_target_sidecar& target =
                            targets[owner.target_index];
                        match_result result =
                            pipe::gpu_coordinate_to_match_result(
                                coord,
                                query_sidecars[query_index].query_len,
                                target.target_len);
                        if (!dispatch_params.do_cigar) {
                            results[query_ids[owner.row_index]].push_back(
                                std::move(result));
                            continue;
                        }

                        const bioseq::pg_types::seq* row_query_hdr =
                            per_row_query_mode
                                ? (*per_row_query_hdrs)[owner.row_index]
                                : query_hdr;
                        const bool recipe_query = gpu_query_recipe_views[
                            owner.row_index].has_value();
                        const bool recipe_target =
                            target.recipe_view.has_value();
                        if ((row_query_hdr == nullptr && !recipe_query)
                            || (target.hdr == nullptr && !recipe_target)) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "GPU CIGAR extension received null query/target sequence");
                        }
                        std::optional<compact_sequence_range<p_sequence>>
                            flat_query_sequence;
                        std::optional<compact_sequence_range<p_sequence>>
                            flat_target_sequence;
                        const gpu_row_view row_query_seq =
                            [recipe_query, &gpu_query_recipe_views,
                             owner_row_index = owner.row_index,
                             &flat_query_sequence, row_query_hdr] {
                            if (recipe_query) {
                                return *gpu_query_recipe_views[
                                    owner_row_index];
                            }
                            flat_query_sequence.emplace(*row_query_hdr);
                            return bioseq::storage::make_lazy_sequence_view(
                                *flat_query_sequence);
                        }();
                        const gpu_row_view target_seq =
                            [recipe_target, &target,
                             &flat_target_sequence] {
                            if (recipe_target) {
                                return *target.recipe_view;
                            }
                            flat_target_sequence.emplace(*target.hdr);
                            return bioseq::storage::make_lazy_sequence_view(
                                *flat_target_sequence);
                        }();
                        const sequence_size_t query_len_bp =
                            row_query_seq.bp_size();
                        const sequence_size_t target_len_bp =
                            target_seq.bp_size();
                        if (result.query_start > query_len_bp
                            || result.target_start > target_len_bp) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
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
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "GPU CIGAR extension received empty coordinate span");
                        }
                        const auto checked_u32 =
                            [] (std::uint64_t value,
                                std::string_view what) -> std::uint32_t {
                            if (value > std::numeric_limits<std::uint32_t>::max()) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
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
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
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
                            bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire{
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
                        pending_cigar_matches.push_back(gpu_cigar_pending_match{
                            .query_id = query_ids[owner.row_index],
                            .match = std::move(result),
                        });
                    }
                    add_stage_host_time(
                        stats,
                        ::bioseq::pipeline_stats::pipeline_stage::cigar_job_generation,
                        gpu_cigar_now_ns() - cigar_prepare_begin_ns);

                    if (dispatch_params.do_cigar && !cigar_jobs.empty()) {
                        const bioseq::gpu::vulkan::gpu_alignment_cigar_params
                            cigar_params{
                                .alphabet_size = static_cast<std::uint32_t>(
                                    compact_sequence_range<p_sequence>::traits::
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
                                        gpu_alignment_cigar_job_wire>{
                                    cigar_jobs.data(),
                                    cigar_jobs.size()},
                                cigar_params,
                                packed_cigar_capacity);
                        add_operational_counter(
                            stats, stats.operational.gpu_extend_dispatches);
                        if (cigar_run.results.size()
                            != pending_cigar_matches.size()) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "GPU CIGAR extension result count mismatch");
                        }
                        for (std::size_t i = 0;
                             i < pending_cigar_matches.size();
                             ++i) {
                            const auto& cigar_result = cigar_run.results[i];
                            const auto& cigar_job = cigar_jobs[i];
                            if (cigar_result.cigar_length == 0u) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                    "GPU CIGAR extension returned empty CIGAR");
                            }
                            const std::uint64_t cigar_end =
                                static_cast<std::uint64_t>(
                                    cigar_result.cigar_offset)
                                + static_cast<std::uint64_t>(
                                    cigar_result.cigar_length);
                            if (cigar_end > cigar_run.packed_cigar.size()) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                    "GPU CIGAR extension returned invalid CIGAR slice");
                            }

                            gpu_cigar_pending_match& pending =
                                pending_cigar_matches[i];
                            match_result& match = pending.match;
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
                                if (opcode == cigar_match_opcode
                                    || opcode == cigar_equal_opcode
                                    || opcode == cigar_mismatch_opcode) {
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
                                } else if (opcode == cigar_insertion_opcode) {
                                    block_length += len;
                                    query_cursor += len;
                                } else if (opcode == cigar_deletion_opcode) {
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
                } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
					/* The persisted grouped-SoA lane is transactional. Its results,
					 * stats, source identities, and lifecycle remain local on
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

            auto& pool = bioseq::parallelism::global_pool::instance();
            /*
             * The persisted hash-major scanner path serializes one
             * complete query at a time because its fragmented target could not
             * enter worker code. For a shared target, authenticate every
             * query/locator/reader identity on the PostgreSQL thread, copy the
             * packed target once under the existing admission authority, then
             * fan independent queries across the task pool. Workers borrow only
             * immutable resident sequence bytes, retained query sketches, and
             * the immutable selective reader; they never call PostgreSQL.
             *
             * task_parallel_region::query_batch owns the outer parallel region,
             * so every inner align_regions/segment region observes an active
             * task_parallel_scope and executes serially within its query worker.
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
                && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
                && pool.maximum_concurrency() > 1
                && batch_size
                    >= bioseq::parallelism::task_parallel_minimums::
                        k_query_count
                && scanner_pg_ctx->cpu_prepared_target != nullptr;
            if (may_parallel_persisted_native) {
                auto* const prepared_target =
                    scanner_pg_ctx->cpu_prepared_target;
                bool batch_identity_valid =
                    prepared_target->unions_ready
                    && prepared_target->target_rowno >= 0
					&& prepared_target->query_count
						>= static_cast<std::uint64_t>(batch_size)
					&& (prepared_target->query_set_validation_ready()
						|| prepared_target->query_count
							== static_cast<std::uint64_t>(batch_size))
                    && prepared_target->target_bp_size
                        > static_cast<std::uint64_t>(
                            postgres_segmentation_threshold_bp);
                const std::int64_t shared_target_rowno =
                    prepared_target->target_rowno;
                const std::uint64_t shared_target_bp_size =
                    prepared_target->target_bp_size;
                const bioseq::storage::sequence_locator* shared_locator = nullptr;
                batch_processor::cpu_prepared_index_group* native_group = nullptr;
                std::vector<const batch_processor::cpu_prepared_query_sketch*>
                    retained_query_sketches(batch_size, nullptr);
                std::vector<minimap2_parameters> native_query_params(batch_size);

                for (std::size_t row = 0;
                     batch_identity_valid && row < batch_size;
                     ++row) {
                    const bioseq::pg_types::seq* const row_query_hdr =
                        (*per_row_query_hdrs)[row];
                    const bioseq::pg_types::seq* const target_hdr = target_hdrs[row];
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
                        || locator->logical_length != shared_target_bp_size
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
                    minimap2_parameters params = dispatch_params;
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
                        || !pipe::minimizer_range_stats_matches_backend(
                            backend,
                            retained->execution_stats,
                            /*require_nonempty_vector_work=*/false,
                            pipe::exact_simd_range_route::
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
                    && (hash_major_parallel_reader_identity_valid(
                            *native_group, shared_target_bp_size)
                        || fragment_parallel_reader_identity_valid(
                            *native_group, shared_target_bp_size));
                if (batch_identity_valid) {
                    const std::size_t letters_per_unit =
                        facade_letters_per_unit;
                    const std::uint64_t target_remainder =
                        shared_target_bp_size % letters_per_unit;
                    const std::size_t padding = static_cast<std::size_t>(
                        target_remainder == 0u
                            ? 0u : letters_per_unit - target_remainder);
                    bool native_batch_completed = false;
                    with_source_provider_pg_target<p_sequence>(
                        *scanner_pg_ctx,
                        *shared_locator,
                        alphabet_code,
                        storage_unit_byte_width,
                        static_cast<std::uint16_t>(padding),
                        nullptr,
                        [shared_target_rowno, shared_target_bp_size,
                         native_group, batch_size, &pool,
                         &native_query_params, &retained_query_sketches,
                         per_row_query_hdrs, scanner_pg_ctx, prepared_target,
                         record_target_operational_outcomes, &stats,
                         broadcast_query_ordinal,
                         &cached_query_minimizer_stats_accounted,
                         &query_ids, &results, &native_batch_completed,
                         storage_unit_byte_width](
                                auto fragmented_target_view,
                                pg_sequence_read_session*) {
                    auto resident =
                        try_materialize_target_for_segment_workers(
                            fragmented_target_view,
                            storage_unit_byte_width,
                            static_cast<std::uint64_t>(
                                shared_target_rowno));
                    if (resident.has_value()) {
                        const auto resident_target_view =
                            resident->owner.view();
                        const auto* const hash_major_reader =
                            native_group->persisted_hash_major_reader
                            ? &*native_group->persisted_hash_major_reader
                            : nullptr;
                        const auto* const fragment_reader =
                            native_group->persisted_fragment_reader
                            ? &*native_group->persisted_fragment_reader
                            : nullptr;
						const auto fragment_lookup_route =
							native_group->persisted_fragment_lookup_route;
						// Compact-direct preparation is itself the exact memory
						// authority. Attempt the complete canonical batch once,
						// without a duplicate occurrence-estimation pass. A typed
						// preparation-time admission failure may retry from wave 0
						// at the configured worker bound below.
						const std::size_t worker_bounded_fragment_wave_size =
							std::max(
								std::size_t{1u},
								std::min(
									batch_size,
									static_cast<std::size_t>(
										pool.maximum_concurrency())));
						const std::size_t fragment_wave_size =
							fragment_reader != nullptr
								&& fragment_lookup_route
									== bioseq::indexing::minimizer_fragment_lookup_route::
										direct_compact_hash_lookup
							? batch_size
							: worker_bounded_fragment_wave_size;
                        std::vector<match_result_collection>
                            native_collections(batch_size);
                        (void)run_aligner_pg_checked(
                            "persisted fragment-native parallel seed wave",
                            [&native_collections,
                             &native_query_params,
                             resident_target_view,
                             &retained_query_sketches,
                             per_row_query_hdrs,
							 hash_major_reader,
							 fragment_reader,
							 fragment_lookup_route,
							 prepared_target,
							 native_group,
							 batch_size,
							 worker_bounded_fragment_wave_size,
							 fragment_wave_size]()
                                    -> match_result_collection {
                                const auto align_hash_major_query =
                                    [&native_collections,
                                     &native_query_params,
                                     resident_target_view,
                                     &retained_query_sketches,
                                     per_row_query_hdrs](
                                            const std::size_t row,
                                            const bioseq::indexing::
                                                hash_major_minimizer_selective_reader&
                                                    target_index) {
                                        compact_sequence_range<p_sequence>
                                            query_seq{
                                                *(*per_row_query_hdrs)[row]};
                                        const auto lazy_query_view =
                                            bioseq::storage::
                                                make_lazy_sequence_view(
                                                    query_seq);
                                        native_collections[row] =
                                            minimap_port_hash_major_owned(
                                                lazy_query_view,
                                                resident_target_view,
                                                retained_query_sketches[row]
                                                    ->minimizers,
                                                target_index,
                                                native_query_params[row],
                                                /*gpu_index_cache=*/nullptr,
                                                bioseq::indexing::
                                                    gpu_index_cache_identity{});
                                    };
                                const auto align_fragment_query =
                                    [&native_collections,
                                     &native_query_params,
                                     resident_target_view,
                                     &retained_query_sketches,
                                     per_row_query_hdrs](
                                            const std::size_t row,
                                            const bioseq::indexing::
                                                prepared_fragment_anchor_batch::
                                                    query_handle& target_index) {
                                        compact_sequence_range<p_sequence>
                                            query_seq{
                                                *(*per_row_query_hdrs)[row]};
                                        const auto lazy_query_view =
                                            bioseq::storage::
                                                make_lazy_sequence_view(
                                                    query_seq);
                                        native_collections[row] =
                                            minimap_port_fragment_query_owned(
                                                lazy_query_view,
                                                resident_target_view,
                                                retained_query_sketches[row]
                                                    ->minimizers,
                                                target_index,
                                                native_query_params[row],
                                                /*gpu_index_cache=*/nullptr,
                                                bioseq::indexing::
                                                    gpu_index_cache_identity{});
                                    };
                                if (hash_major_reader != nullptr) {
                                    bioseq::parallelism::
                                        parallel_for_pipeline(
                                            0,
                                            batch_size,
                                            1,
                                            bioseq::parallelism::
                                                task_parallel_minimums::
                                                    k_query_count,
                                            bioseq::parallelism::
                                                task_parallel_region::
                                                    query_batch,
                                            [&align_hash_major_query,
                                             hash_major_reader](
                                                    const std::size_t lo,
                                                    const std::size_t hi) {
                                                for (std::size_t row = lo;
                                                     row < hi;
                                                     ++row) {
                                                    align_hash_major_query(
                                                        row,
                                                        *hash_major_reader);
                                                }
                                            });
                                    return {};
                                }
                                if (fragment_reader == nullptr) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
                                        "fragment-native grouped-SoA reader disappeared");
                                }

                                auto& memory_admission = bioseq::parallelism::
                                    memory_admission_controller::instance();
                                const std::uint64_t first_wave_prepare_started_ns =
                                    dispatch_host_now_ns();
                                auto scratch_result = bioseq::indexing::
                                    query_wave_scratch::create(memory_admission);
                                if (!scratch_result) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
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
                                    bioseq::common::saturating_mul(
                                        fragment_wave_size,
                                        sizeof(bioseq::indexing::
                                            fragment_seed_query));
                                const std::uint64_t descriptor_headroom =
                                    bioseq::common::saturating_mul(
                                        descriptor_bytes,
                                        bioseq::parallelism::
                                            k_vector_preallocation_headroom_factor);
                                if (descriptor_bytes
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()
                                    || descriptor_headroom
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
                                            "fragment-native query descriptor "
                                            "capacity overflow");
                                }
                                auto descriptor_permit_result =
                                    memory_admission.try_reserve(
                                        descriptor_headroom);
                                if (!descriptor_permit_result) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
                                            "fragment-native query descriptors "
                                            "were not admitted");
                                }
                                // Declared before the vector so reverse
                                // destruction releases storage before permit.
                                auto descriptor_permit =
                                    std::move(*descriptor_permit_result);
                                std::vector<bioseq::indexing::fragment_seed_query>
                                    wave_queries;
                                try {
                                    wave_queries.reserve(fragment_wave_size);
                                } catch (const std::bad_alloc&) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
                                            "fragment-native query descriptor "
                                            "allocation failed");
                                }
                                const std::uint64_t actual_descriptor_bytes =
                                    bioseq::common::saturating_mul(
                                        wave_queries.capacity(),
                                        sizeof(bioseq::indexing::
                                            fragment_seed_query));
                                if (actual_descriptor_bytes
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()
                                    || (actual_descriptor_bytes
                                            > descriptor_permit.bytes()
                                        && !descriptor_permit.grow_to(
                                            actual_descriptor_bytes))) {
                                    throw bioseq::indexing::
                                        persisted_minimizer_lookup_error(
                                            "fragment-native query descriptor "
                                            "reconciliation failed");
                                }
                                descriptor_permit.shrink_to(
                                    actual_descriptor_bytes);

								std::size_t active_fragment_wave_size =
									fragment_wave_size;
								bool full_wave_admission_retry_available =
									fragment_lookup_route
										== bioseq::indexing::
											minimizer_fragment_lookup_route::
												direct_compact_hash_lookup
									&& fragment_wave_size == batch_size
									&& worker_bounded_fragment_wave_size
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
											+ active_fragment_wave_size);
                                    wave_queries.clear();
                                    for (std::size_t row = wave_begin;
                                         row < wave_end;
                                         ++row) {
                                        wave_queries.push_back(
                                            bioseq::indexing::fragment_seed_query{
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
										fragment_anchor_wave_completion
											wave_completion{query_wave_scratch};
										std::optional<bioseq::indexing::
											prepared_fragment_anchor_batch>
												prepared_seed_batch;
										try {
											prepared_seed_batch.emplace(
												fragment_reader->
													prepare_fragment_anchor_batch(
														memory_admission,
														std::span<const bioseq::indexing::
															fragment_seed_query>{
																wave_queries},
														native_query_params[
															wave_begin].max_occ,
														query_wave_scratch,
														/*baseline_query_plan_matcher_for_testing=*/false,
														/*verify_compositional_order_for_testing=*/false,
														fragment_lookup_route));
										} catch (const bioseq::indexing::
												persisted_minimizer_lookup_error& error) {
											retry_at_worker_bound =
												wave_begin == 0u
												&& full_wave_admission_retry_available
												&& error.failure_reason()
												== bioseq::indexing::
													persisted_minimizer_lookup_error::
														reason::
																admission_or_allocation;
										if (!retry_at_worker_bound) throw;
									}

									if (!retry_at_worker_bound) {
										if (native_group->
												compact_direct_query_set_validation_pending) {
											const bool complete_query_set =
												wave_begin == 0u
												&& wave_end == batch_size
												&& wave_queries.size() == batch_size;
											if (!complete_query_set
												|| !batch_processor::
													publish_compact_direct_query_set_stats(
														*prepared_target,
														*native_group,
														prepared_seed_batch->
																			compact_direct_query_stats())) {
												throw bioseq::indexing::
													persisted_minimizer_lookup_error(
												"compact-direct full-batch query-set stats were rejected");
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
												task_parallel_minimums::
													k_query_count,
													bioseq::parallelism::
												task_parallel_region::
															query_batch,
											[&align_fragment_query,
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
													align_fragment_query(
														row,
																query_index);
														}
													});
											native_collections[wave_begin].
												record_fragment_native_query_stats(
													prepared_seed_batch->stats());
											native_collections[wave_begin].
												record_seed_lookup_host_time(
													seed_wave_prepare_host_wall_ns);
										}
									}
									if (retry_at_worker_bound) {
										full_wave_admission_retry_available = false;
										if (query_wave_scratch.wave_outstanding()) {
											throw bioseq::indexing::
												persisted_minimizer_lookup_error(
													"fragment-native failed wave retained "
													"active scratch state");
										}
										query_wave_scratch.
											release_retained_capacity();
										batch_processor::
											finalize_pending_compact_direct_query_set_validation_fallback(
												*prepared_target);
										active_fragment_wave_size =
											worker_bounded_fragment_wave_size;
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
                            attach_retained_hash_major_query_stats(
                                native_collections[row],
                                retained_query_sketches[row]->execution_stats);
                            scanner_pg_ctx->target_index_sources[row] =
                                ::bioseq::pipeline_stats::
                                    target_index_source::persisted;
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
                            bioseq::postgres::support::invoke_pg_as_cpp(
                                [] { CHECK_FOR_INTERRUPTS(); });
                            if (record_target_operational_outcomes) {
                                record_target_row(stats);
                                record_target_index_outcome(
                                    stats,
                                    requested_source,
                                    ::bioseq::pipeline_stats::
                                        target_index_source::persisted);
                            }
                            record_unattested_job(
                                stats,
                                native_query_params[row],
                                requested_source,
                                ::bioseq::pipeline_stats::
                                    target_index_source::persisted,
                                scanner_pg_ctx,
                                row,
                                /*per_row_query_mode=*/true,
                                broadcast_query_ordinal,
                                &native_collections[row],
                                /*consumed_shared_cached_query_stats=*/false,
                                cached_query_minimizer_stats_accounted);
                            bioseq_log<logging_level::debug>(
                                "[bioseq] pg: batch_processor row query_id={} "
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
                            if (!native_collections[row].is_empty()) {
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
					query_set_validation_ready()) {
				(void)run_aligner_pg_checked(
					"compact-direct query-set slow fallback",
					[prepared_target = scanner_pg_ctx->cpu_prepared_target]()
							-> match_result_collection {
						batch_processor::
							finalize_pending_compact_direct_query_set_validation_fallback(
								*prepared_target);
						return {};
					});
			}

			std::vector<std::uint8_t> parallel_done;
            std::vector<match_result_collection> parallel_collections;
            std::vector<minimap2_parameters> parallel_params;

            const bool may_dispatch_parallel =
                !dispatch_params.enable_profiling
                && !selected_vulkan_accelerator(dispatch_params)
                && !use_scanner_pg
                && !chunked_scanner_forced
                && bioseq::parallelism::has_parallel_dispatch(pool.get_mode())
                && pool.maximum_concurrency() > 1
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
                            const bioseq::pg_types::seq* row_query_hdr =
                                per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                            const bioseq::pg_types::seq* target_hdr = target_hdrs[i];
                            if (row_query_hdr == nullptr || target_hdr == nullptr) {
                                continue;
                            }
                            if (row_query_hdr->alphabet_code != alphabet_code
                                || row_query_hdr->storage_unit_byte_width != storage_unit_byte_width
                                || target_hdr->alphabet_code != alphabet_code
                                || target_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                                continue;
                            }
                            if (target_hdr->bp_size > postgres_segmentation_threshold_bp) {
                                continue;
                            }
                            const size_t min_bp = std::min<size_t>(
                                static_cast<size_t>(row_query_hdr->bp_size),
                                static_cast<size_t>(target_hdr->bp_size));
                            minimap2_parameters params = apply_size_heuristic(dispatch_params, min_bp);
                            parallel_collections[i] =
                                process_inmemory_row(
                                    row_query_hdr,
                                    target_hdr,
                                    static_cast<sequence_size_t>(target_hdr->bp_size),
                                    params,
                                    false);
                            parallel_params[i] = params;
                            parallel_done[i] = 1;
                        }
                    }});
            }

            for (size_t i = 0; i < batch_size; ++i) {
                // Cancellation point at the per-row boundary. The
                // native aligner itself is NO_POSTGRES and cannot call
                // CHECK_FOR_INTERRUPTS; this lets a multi-row mmsearch honour
                // pg_cancel_backend between rows, and (with the storage-layer
                // check in fetch_payload) bounds how long a cancel waits.
				bioseq::postgres::support::invoke_pg_as_cpp(
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
                            ::bioseq::pipeline_stats::target_index_source::recomputed);
						shared_target_outcome_recorded = true;
                    }
                    record_unattested_job(
                        stats,
                        parallel_params[i],
                        requested_source,
                        ::bioseq::pipeline_stats::target_index_source::recomputed,
                        scanner_pg_ctx,
                        i,
                        per_row_query_mode,
                        broadcast_query_ordinal,
                        &parallel_collections[i],
                        /*consumed_shared_cached_query_stats=*/false,
                        cached_query_minimizer_stats_accounted);
                    const sequence_size_t target_bp_size =
                        static_cast<sequence_size_t>(target_hdrs[i]->bp_size);
                    const std::size_t match_count = parallel_collections[i].size();
                    bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es) [dispatch-parallel]",
                            static_cast<int>(query_ids[i]),
                            target_bp_size,
                            parallel_params[i].kmer_size,
                            parallel_params[i].window_size,
                            parallel_params[i].min_chain_score,
                            parallel_params[i].min_seed_count,
                            false, match_count);
                    if (!parallel_collections[i].is_empty()) {
                        results[query_ids[i]] = std::move(parallel_collections[i].results);
                    }
                    continue;
                }
                const bioseq::pg_types::seq* row_query_hdr =
                    per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                const auto* query_locator = use_scanner_pg
                    && per_row_query_mode
                    && scanner_pg_ctx->query_locators[i].has_value()
                    ? std::addressof(*scanner_pg_ctx->query_locators[i])
                    : nullptr;
                const bool has_query_locator_for_pg = query_locator != nullptr;
                const Datum query_recipe = use_scanner_pg
                    && per_row_query_mode
                    ? scanner_pg_ctx->query_recipe_datums[i]
                    : Datum{0};
                const bool has_query_recipe = query_recipe != Datum{0};
                const Datum target_recipe = use_scanner_pg
                    ? scanner_pg_ctx->target_recipe_datums[i]
                    : Datum{0};
                const bool has_target_recipe = target_recipe != Datum{0};
                if (static_cast<unsigned>(row_query_hdr != nullptr)
                        + static_cast<unsigned>(has_query_locator_for_pg)
                        + static_cast<unsigned>(has_query_recipe) > 1u) {
                    raise_typed_dispatch_pg_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "query recipe/locator/flat dispatch ownership overlaps");
                }
                if (row_query_hdr == nullptr && !has_query_locator_for_pg
                    && !has_query_recipe) {
                    continue;
                }
                if (row_query_hdr != nullptr
                    && (row_query_hdr->alphabet_code != alphabet_code
                        || row_query_hdr->storage_unit_byte_width
                            != storage_unit_byte_width)) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: query row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(row_query_hdr->alphabet_code),
                            static_cast<unsigned>(row_query_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                if (has_query_locator_for_pg
                    && (scanner_pg_ctx->mmsearch_query_attno <= 0
                        || scanner_pg_ctx->query_alphabet_code != alphabet_code
                        || !(scanner_pg_ctx->query_storage_unit_byte_width == 1u
                            || scanner_pg_ctx->query_storage_unit_byte_width == 2u
                            || scanner_pg_ctx->query_storage_unit_byte_width == 4u
                            || scanner_pg_ctx->query_storage_unit_byte_width == 8u))) {
                    raise_typed_dispatch_pg_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "per-row query locator metadata is invalid");
                }
                if (has_query_recipe) {
                    const auto& descriptor =
                        scanner_pg_ctx->query_recipe_descriptors[i];
                    const bool expanded =
                        descriptor.kind
                            == pg_sequence_datum_kind::expanded_read_only
                        || descriptor.kind
                            == pg_sequence_datum_kind::expanded_read_write;
                    if (!VARATT_IS_EXTERNAL_EXPANDED(
                            DatumGetPointer(query_recipe))
                        || !expanded || !descriptor.alphabet_bound
                        || descriptor.alphabet_code != alphabet_code
                        || descriptor.storage_unit_byte_width
                            != scanner_pg_ctx->query_storage_unit_byte_width) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "per-row query recipe metadata is invalid");
                    }
                }
                const bioseq::pg_types::seq* target_hdr = target_hdrs[i];
                // When use_scanner_pg=true and a locator was captured
                // for this row (e.g. a stub whose materialised varlena would
                // exceed MaxAllocSize, so hydration was skipped), we still have
                // enough info to dispatch via scanner_pg's PG-backed mode. Only
                // skip when NEITHER an inline target nor a locator is available.
                const bool has_locator_for_pg =
                    use_scanner_pg
                    && scanner_pg_ctx->target_locators[i].has_value();
                if (static_cast<unsigned>(target_hdr != nullptr)
                        + static_cast<unsigned>(has_locator_for_pg)
                        + static_cast<unsigned>(has_target_recipe) > 1u) {
                    raise_typed_dispatch_pg_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "target recipe/locator/flat dispatch ownership overlaps");
                }
                if (target_hdr == nullptr && !has_locator_for_pg
                    && !has_target_recipe) {
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
                    bioseq_log<logging_level::debug>("[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(target_hdr->alphabet_code),
                            static_cast<unsigned>(target_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                if (has_target_recipe) {
                    const auto& descriptor =
                        scanner_pg_ctx->target_recipe_descriptors[i];
                    const bool expanded =
                        descriptor.kind
                            == pg_sequence_datum_kind::expanded_read_only
                        || descriptor.kind
                            == pg_sequence_datum_kind::expanded_read_write;
                    if (!VARATT_IS_EXTERNAL_EXPANDED(
                            DatumGetPointer(target_recipe))
                        || !expanded || !descriptor.alphabet_bound
                        || descriptor.alphabet_code != alphabet_code
                        || descriptor.storage_unit_byte_width
                            != storage_unit_byte_width) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "target recipe metadata is invalid");
                    }
                }
                // Construct compact_sequence_range only when target_hdr is
                // present. With a PG-backed locator we don't materialise the
                // full payload — scanner_pg streams fragment-by-fragment. Use
                // the locator's logical_length for the heuristic-relevant
                // min_bp comparison.
                const sequence_size_t target_bp_size = target_hdr != nullptr
                    ? static_cast<sequence_size_t>(target_hdr->bp_size)
                    : has_target_recipe
                        ? static_cast<sequence_size_t>(
                            scanner_pg_ctx->target_recipe_descriptors[i].
                                bp_length)
                        : scanner_pg_ctx->target_locators[i]->logical_length;

                const sequence_size_t row_query_bp_size =
                    has_query_locator_for_pg
                        ? query_locator->logical_length
                        : has_query_recipe
                            ? static_cast<sequence_size_t>(
                                scanner_pg_ctx->query_recipe_descriptors[i].
                                    bp_length)
                            : static_cast<sequence_size_t>(
                                row_query_hdr->bp_size);
                const size_t min_bp = std::min<size_t>(
                    static_cast<size_t>(row_query_bp_size),
                    static_cast<size_t>(target_bp_size));

                minimap2_parameters params = apply_size_heuristic(dispatch_params, min_bp);
                match_result_collection collection;
                bool consumed_shared_cached_query_stats = false;
                {
                    if (use_scanner_pg) {
                        using row_alphabet = typename
                            compact_sequence_range<p_sequence>::traits::alphabet_type;
                        using row_view = bioseq::storage::
                            lazy_sequence_view<row_alphabet>;
                        auto execute_scanner =
                            [target_hdr, i, target_bp_size, min_bp,
                             &dispatch_params, &params, per_row_query_mode,
                             &cached_query_minimizers,
                             &cached_query_minimizer_stats,
                             &consumed_shared_cached_query_stats,
                             alphabet_code, storage_unit_byte_width,
                             scanner_pg_ctx, &pool](
                                row_view query_lazy_view,
                                std::optional<row_view> target_lazy_view,
                                pg_sequence_read_session* active_session) {
                        return process_scanner_pg_row(
                            query_lazy_view,
                            target_lazy_view,
                            target_hdr,
                            i,
                            target_bp_size,
                            min_bp,
                            dispatch_params,
                            params,
                            per_row_query_mode,
                            cached_query_minimizers,
                            cached_query_minimizer_stats,
                            consumed_shared_cached_query_stats,
                            alphabet_code,
                            storage_unit_byte_width,
                            scanner_pg_ctx,
                            pool,
                            active_session);
                        };
                        auto execute_with_optional_target_recipe =
                            [has_target_recipe, &collection,
                             &execute_scanner, target_recipe,
                             scanner_pg_ctx, i](
                                row_view query_lazy_view,
                                pg_sequence_read_session* active_session) {
                            if (!has_target_recipe) {
                                collection = execute_scanner(
                                    query_lazy_view, std::nullopt,
                                    active_session);
                                return;
                            }
                            if (active_session == nullptr) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_INTERNAL_ERROR,
                                    "target recipe has no shared read session");
                            }
                            const auto target_descriptor = scanner_pg_ctx->
                                target_recipe_descriptors[i];
                            auto* target_owner =
                                bioseq::postgres::support::invoke_pg_as_cpp(
                                    [active_session, target_recipe,
                                     target_descriptor] {
                                        return ::make_pg_sequence_call_owner<
                                            row_alphabet>(
                                                *active_session,
                                                target_recipe,
                                                target_descriptor);
                                    });
                            collection = execute_scanner(
                                query_lazy_view,
                                std::optional<row_view>{
                                    target_owner->view()},
                                active_session);
                        };
                        if (has_query_recipe) {
                            with_pg_sequence_recipe<row_alphabet>(
                                *scanner_pg_ctx,
                                query_recipe,
                                scanner_pg_ctx->
                                    query_recipe_descriptors[i],
                                scanner_pg_ctx->query_alphabet_code,
                                scanner_pg_ctx->
                                    query_storage_unit_byte_width,
                                [&execute_with_optional_target_recipe](
                                        row_view query_lazy_view,
                                        pg_sequence_read_session*
                                            active_session) {
                                    execute_with_optional_target_recipe(
                                        query_lazy_view, active_session);
                                });
                        } else if (has_query_locator_for_pg) {
                            const std::size_t query_letters_per_unit =
                                bioseq::storage::packed_letters_per_unit(
                                    scanner_pg_ctx->
                                        query_storage_unit_byte_width,
                                    static_cast<std::uint8_t>(
                                        p_sequence::letter_bit_width));
                            if (query_letters_per_unit == 0u) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_DATA_CORRUPTED,
                                    "per-row query locator has invalid packed geometry");
                            }
                            const std::uint64_t query_remainder =
                                query_locator->logical_length
                                    % query_letters_per_unit;
                            const std::uint64_t query_padding =
                                query_remainder == 0u
                                    ? 0u
                                    : query_letters_per_unit
                                        - query_remainder;
                            if (query_padding
                                > (std::numeric_limits<std::uint16_t>::max)()) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_DATA_CORRUPTED,
                                    "per-row query locator padding is invalid");
                            }
                            with_source_provider_pg_locator<row_alphabet>(
                                *scanner_pg_ctx,
                                *query_locator,
                                static_cast<AttrNumber>(
                                    scanner_pg_ctx->mmsearch_query_attno),
                                scanner_pg_ctx->query_alphabet_code,
                                scanner_pg_ctx->
                                    query_storage_unit_byte_width,
                                static_cast<std::uint16_t>(query_padding),
                                scanner_pg_ctx->read_session,
                                [&execute_with_optional_target_recipe](
                                        row_view query_lazy_view,
                                        pg_sequence_read_session*
                                            active_session) {
                                    execute_with_optional_target_recipe(
                                        query_lazy_view, active_session);
                                });
                        } else {
                            compact_sequence_range<p_sequence> query_seq{
                                *row_query_hdr};
                            const row_view query_lazy_view =
                                bioseq::storage::make_lazy_sequence_view(
                                    query_seq);
                            if (has_target_recipe) {
                                with_pg_sequence_recipe<row_alphabet>(
                                    *scanner_pg_ctx,
                                    target_recipe,
                                    scanner_pg_ctx->
                                        target_recipe_descriptors[i],
                                    alphabet_code,
                                    storage_unit_byte_width,
                                    [&collection, &execute_scanner,
                                     query_lazy_view](
                                            row_view target_lazy_view,
                                            pg_sequence_read_session*
                                                active_session) {
                                        collection = execute_scanner(
                                            query_lazy_view,
                                            std::optional<row_view>{
                                                target_lazy_view},
                                            active_session);
                                    });
                            } else {
                                collection = execute_scanner(
                                    query_lazy_view, std::nullopt,
                                    scanner_pg_ctx->read_session);
                            }
                        }
                    } else if (target_hdr != nullptr) {
                        collection =
                            process_inmemory_row(
                                row_query_hdr,
                                target_hdr,
                                target_bp_size,
                                params,
                                chunked_scanner_forced);
                    } else if (scanner_pg_ctx != nullptr
                               && scanner_pg_ctx->target_locators[i].has_value()) {
                        // inmemory_forced=ON cannot satisfy its
                        // force-baseline semantic on > MaxAllocSize stubs; raise.
						const int query_id = static_cast<int>(query_ids[i]);
						bioseq::postgres::support::invoke_pg_as_cpp(
							[query_id, target_bp_size] {
								ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("forced in-memory scanner cannot materialize target stub"),
									 errdetail("row=%d target_bp=%llu exceeds MaxAllocSize",
										query_id,
										static_cast<unsigned long long>(target_bp_size)),
								 errhint("Set bioseq.testing.in_memory_scanner.enabled=off to use the streaming scanner_pg path.")));
							});
						pg_unreachable();
                    } else {
                        bioseq_log<logging_level::debug>("[bioseq] pg: row {} has no inline target and no scanner_pg locator; treating as WHERE FALSE",
                                static_cast<int>(query_ids[i]));
                        continue;
                    }
                }

                const auto target_index_source = use_scanner_pg
                    ? scanner_pg_ctx->target_index_sources[i]
                    : ::bioseq::pipeline_stats::target_index_source::recomputed;
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
                    consumed_shared_cached_query_stats,
                    cached_query_minimizer_stats_accounted);

                bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
                        static_cast<int>(query_ids[i]),
                        target_bp_size,
                        params.kmer_size,
                        params.window_size,
                        params.min_chain_score,
                        params.min_seed_count,
                        target_index_source
                            == ::bioseq::pipeline_stats::target_index_source::persisted,
                        collection.size());
                if (!collection.is_empty()) {
                    results[query_ids[i]] = std::move(collection.results);
                }
            }
    return results;
}

} // namespace

[[bioseq::template_flatten_when("shape", "scanner_pg")]]
[[bioseq::template_flatten_entry("scanner_pg")]]
[[nodiscard]] match_result_collection
process_scanner_pg_row(
    bioseq::storage::lazy_sequence_view<
        alphabet> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        alphabet>> target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    const minimap2_parameters& dispatch_params,
    const minimap2_parameters& params,
    const bool per_row_query_mode,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>&
        cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_scanner_pg_row_typed<sequence_type>(
        query_view,
        target_recipe_view,
        target_hdr,
        i,
        target_bp_size,
        min_bp,
        dispatch_params,
        params,
        per_row_query_mode,
        cached_query_minimizers,
        cached_query_minimizer_stats,
        consumed_shared_cached_query_stats,
        alphabet_code,
        storage_unit_byte_width,
        scanner_pg_ctx,
        pool,
        active_read_session);
}

[[bioseq::template_flatten_when("shape", "inmemory")]]
[[bioseq::template_flatten_entry("inmemory")]]
[[nodiscard]] match_result_collection
process_inmemory_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const sequence_size_t target_bp_size,
    const minimap2_parameters& params,
    const bool chunked_scanner_forced)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        target_bp_size,
        params,
        chunked_scanner_forced);
}

[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[bioseq::template_flatten_entry("inmemory_standard")]]
[[nodiscard]] match_result_collection
process_inmemory_standard_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_standard_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[bioseq::template_flatten_entry("inmemory_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[bioseq::template_flatten_entry("inmemory_chunked")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[bioseq::template_flatten_entry("inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
[[nodiscard]] std::map<int32_t, std::vector<match_result>>
process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& base_params,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    const auto& typmod_operations =
        cpu_preparation_detail::typmod_ops<alphabet, storage_width>();
    if (!typmod_operations.matches(
            alphabet_code, storage_unit_byte_width)) {
        throw std::logic_error{
            "generated scheduler typmod operations disagree with the selected root"};
    }
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->highway_preparation.valid()) {
        auto preparation = std::exchange(
            scanner_pg_ctx->highway_preparation,
            batch_processor::scanner_pg_context::highway_preparation_hook{});
        preparation.function(
            preparation.state,
            *scanner_pg_ctx,
            base_params,
            typmod_operations);
        if (scanner_pg_ctx->cpu_prepared_target == nullptr) {
            throw std::logic_error{
                "Highway preparation hook did not publish a CPU target"};
        }
    }
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->cpu_prepared_target != nullptr
        && (!scanner_pg_ctx->cpu_prepared_target->unions_ready
            || !scanner_pg_ctx->cpu_prepared_target
                ->highway_binding_matches(base_params))) {
        throw std::logic_error{
            "prepared CPU target disagrees with the selected Highway binding"};
    }

    const auto run_pipeline = [&] {
        return process_sequences_typed<sequence_type>(
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
    if (!base_params.enable_profiling) {
        return run_pipeline();
    }

    /* The one-shot summary still fires at scan end. Preparation remains
     * outside this scope, matching the authored root's previous ordering. */
    using profiling_arguments = bioseq::instrumentation::production_profiling_configuration;
    using timing_arguments = bioseq::instrumentation::benchmark_profiling_configuration;
    auto& profiler_block = bioseq::instrumentation::persistent_profiler_state<profiling_arguments>();
    auto& timing_profiler_block =
        bioseq::instrumentation::persistent_profiler_state<timing_arguments>();
    bioseq::instrumentation::installed_profiler_scope<profiling_arguments> guard(profiler_block);
    bioseq::instrumentation::installed_profiler_scope<timing_arguments> timing_guard(
        timing_profiler_block);
    static const auto* batch_desc = bioseq::instrumentation::register_static_region(
        "batch_processor.process_sequences");
    [[maybe_unused]] auto batch_scope = bioseq::instrumentation::region<profiling_arguments>(batch_desc);
    return run_pipeline();
}

} // namespace typed_dispatch
} // namespace bioseq::simd
