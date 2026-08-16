/**
 * @file
 * @brief Declares batch processing for sequence dispatch.
 */
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
 * - `bioseq.index.memory_cap_mb`: memory-admission cap for resident targets,
 *   assembled indexes, sorted runs, and opportunistic RAM-first builds. `0`
 *   means use the live process/system memory budget. A larger or smaller cap
 *   can admit or reject resident/parallel paths, but it does not force
 *   persistence and it does not choose the active index.
 *
 * - `bioseq.scheduler.pipeline.mode` and
 *   `bioseq.scheduler.backend.max_concurrency`: enable task-parallel segment work when
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
module;

#include <bioseq/pg_compat.h>

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
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.storage;
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
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
namespace bioseq::simd {

/** @brief Maximum number of rows processed by one scheduler dispatch batch. */
export constexpr size_t dispatch_batch_capacity = 8192;

/**
 * @brief Derives a nonzero diagnostic token from a hash-major source identity.
 *
 * Existing pipeline stats have a frozen u64 source field, while hash-major
 * authenticates a 128-bit source identity.  This domain-separated token is
 * diagnostic only: every hash-major correctness decision retains and compares
 * all 128 authenticated bits.
 *
 * @param[in] source_identity Authenticated 128-bit source identity.
 * @return A stable nonzero token used only in diagnostic statistics.
 */
export [[nodiscard]] constexpr std::uint64_t
hash_major_source_diagnostic_token(const std::array<std::byte, 16>& source_identity) noexcept {
    std::array<std::byte, 32> bytes{std::byte{'B'},
                                    std::byte{'S'},
                                    std::byte{'E'},
                                    std::byte{'Q'},
                                    std::byte{'F'},
                                    std::byte{'6'},
                                    std::byte{'D'},
                                    std::byte{'I'},
                                    std::byte{'A'},
                                    std::byte{'G'},
                                    std::byte{'v'},
                                    std::byte{'1'},
                                    std::byte{0},
                                    std::byte{0},
                                    std::byte{0},
                                    std::byte{0}};
    for (std::size_t index = 0; index < source_identity.size(); ++index) {
        bytes[16u + index] = source_identity[index];
    }
    const std::uint64_t token = bioseq::indexing::crc64_ecma182(bytes);
    return token == 0u ? 0x9e3779b97f4a7c15ull : token;
}

namespace {
constexpr std::array<std::byte, 16> hash_major_diagnostic_identity_a{std::byte{1}};
constexpr std::array<std::byte, 16> hash_major_diagnostic_identity_b{std::byte{2}};
static_assert(hash_major_source_diagnostic_token(std::array<std::byte, 16>{}) != 0u);
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) != 0u);
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) ==
              hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a));
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) !=
              hash_major_source_diagnostic_token(hash_major_diagnostic_identity_b));
} // namespace
/**
 * @brief Selects whether a batch is the primary query or an additional query.
 */
export enum class batch_invocation_role : std::uint8_t {
    primary,
    extra_query,
};

/**
 * @brief Prepares and executes typed minimap2 work for one scheduler batch.
 *
 * `batch_processor` drives minimap_port across a batch of query and target
 * pairs that all share the same (alphabet_code, storage_unit_byte_width) typmod. The
 * caller hands us the already-detoasted query varlena (captured at
 * BeginCustomScan — see columnar_customscan.cppm) and an array of per-row
 * target varlenas (captured at submit_scan — see tuple_dispatch_buffer).
 *
 * Dispatch: we branch on (alphabet_code, storage_unit_byte_width) exactly once, then
 * inside the matching arm all eight compact_sequence_range<p_sequence>
 * specialisations compile down to their own minimap_port instantiation.
 * No std::variant, no runtime type erasure on the hot path.
 *
 * Execution validation is owned by the target-independent canonical value in
 * bioseq::pipeline_stats. Do not reintroduce a Highway-local stats owner.
 */
export class batch_processor {
  public:
    /** @brief Selects the retained representation of a prepared CPU query union. */
    enum class cpu_query_union_storage : std::uint8_t {
        minimizer_sketch,
        canonical_u32,
        canonical_u32_sequential_fallback,
        retained_sketch_only,
    };

    /** @brief Owns one independently prepared query sketch and its admission. */
    struct cpu_prepared_query_sketch {
        std::uint64_t query_ordinal = 0;
        std::uint64_t query_bp_size = 0;
        /* During parallel preparation each query owns an independent permit.
         * The backend thread absorbs it into the target aggregate before
         * publication.  It is declared before minimizers so exceptional
         * teardown deallocates vector storage before releasing admission. */
        std::optional<bioseq::parallelism::memory_permit> parallel_preparation_permit{};
        std::vector<minimap_sketch_data> minimizers{};
        pipe::minimizer_range_execution_stats execution_stats{};
    };

    /** @brief Reports the terminal state of independent CPU query preparation. */
    enum class cpu_preparation_status : std::uint8_t {
        pending,
        ready,
        query_length_unsupported,
        admission_unavailable,
        collector_growth_failed,
        capacity_mismatch,
        validation_failed,
        unsupported_typmod,
    };

    /** @brief Function signature for appending a flat query into a union. */
    template <class p_destination>
    using cpu_append_header_function =
        void (*)(const bioseq::pg_types::seq&,
                 std::uint64_t,
                 std::int32_t,
                 std::int32_t,
                 minimizer_sketch_backend,
                 const ::bioseq::simd_dispatch::highway_kernel_table*,
                 std::vector<p_destination>&,
                 bioseq::parallelism::memory_permit&,
                 std::uint64_t&,
                 std::vector<cpu_prepared_query_sketch>*,
                 bioseq::parallelism::memory_permit*,
                 std::uint64_t*);

    /** @brief Function signature for appending a recipe-backed query into a union. */
    template <class p_destination>
    using cpu_append_recipe_function =
        void (*)(Datum,
                 const pg_sequence_descriptor&,
                 pg_sequence_read_session&,
                 const bioseq::storage::sequence_payload_allocation_observer*,
                 std::uint64_t,
                 std::int32_t,
                 std::int32_t,
                 minimizer_sketch_backend,
                 const ::bioseq::simd_dispatch::highway_kernel_table*,
                 std::vector<p_destination>&,
                 bioseq::parallelism::memory_permit&,
                 std::uint64_t&,
                 std::vector<cpu_prepared_query_sketch>*,
                 bioseq::parallelism::memory_permit*,
                 std::uint64_t*);

    /** @brief Function signature for preparing one independent query sketch. */
    using cpu_prepare_independent_function =
        cpu_preparation_status (*)(const bioseq::pg_types::seq&,
                                   std::uint64_t,
                                   std::int32_t,
                                   std::int32_t,
                                   minimizer_sketch_backend,
                                   const ::bioseq::simd_dispatch::highway_kernel_table*,
                                   cpu_prepared_query_sketch&);

    /** @brief Binds typmod-specific query preparation callables without runtime erasure. */
    struct cpu_preparation_typmod_operations final {
        std::uint8_t alphabet_code = 0;
        std::uint8_t storage_unit_byte_width = 0;
        cpu_append_header_function<minimap_sketch_data> append_header_sketch = nullptr;
        cpu_append_header_function<std::uint32_t> append_header_u32 = nullptr;
        cpu_append_recipe_function<minimap_sketch_data> append_recipe_sketch = nullptr;
        cpu_append_recipe_function<std::uint32_t> append_recipe_u32 = nullptr;
        cpu_prepare_independent_function prepare_independent = nullptr;

        /** @brief Returns whether every required typmod callable is bound. */
        [[nodiscard]] constexpr bool complete() const noexcept {
            return std::has_single_bit(storage_unit_byte_width) &&
                   append_header_sketch != nullptr && append_header_u32 != nullptr &&
                   append_recipe_sketch != nullptr && append_recipe_u32 != nullptr &&
                   prepare_independent != nullptr;
        }

        /** @brief Returns whether the complete table matches an alphabet and width. */
        [[nodiscard]] constexpr bool matches(const std::uint8_t alphabet,
                                             const std::uint8_t width) const noexcept {
            return complete() && alphabet_code == alphabet && storage_unit_byte_width == width;
        }
    };

    static_assert(std::is_standard_layout_v<cpu_preparation_typmod_operations>);
    static_assert(std::is_trivially_copyable_v<cpu_preparation_typmod_operations>);

    /** @brief Owns one prepared CPU index parameter group for a shared target. */
    struct cpu_prepared_index_group {
        std::int32_t kmer_size = 0;
        std::int32_t window_size = 0;
        std::int32_t max_occ = 0;
        minimizer_sketch_backend sketch_backend = minimizer_sketch_backend::scalar;
        std::vector<minimap_sketch_data> query_hash_union{};
        /* Hash-major borrows this exact sorted-unique canonical image for its
         * complete lifetime.  It is mutually exclusive with query_hash_union:
         * publication is authenticated before the scheduler chooses which
         * representation to construct, so there is never a second Q-sized
         * conversion owner or sort.  Declaring it before the reader makes
         * reverse destruction release the borrowing reader first. */
        std::vector<std::uint32_t> canonical_query_hash_union_u32{};
        /* Hash-major preparation may retain the exact per-query sketch produced
         * while filling canonical_query_hash_union_u32.  Missing ordinals are
         * intentional admission fallbacks and are sketched by ensure_q in the
         * typed row path. */
        std::vector<cpu_prepared_query_sketch> retained_query_sketches{};
        /* Declared before target_index so destruction releases the owned
         * index storage before returning its admission reservation. */
        std::optional<bioseq::parallelism::memory_permit> target_index_permit{};
        std::shared_ptr<bioseq::indexing::minimizer_hashmap_fragment_index> target_index{};
        /* Production minimizer-fragment grouped-SoA keeps every physical-fragment index
         * independent. Its only target-wide state is query-specific global
         * occurrence counts required by max-occ pruning; no posting owner,
         * span federation, or hash-to-fragment lookup index is retained. */
        /* Declared before the borrowing reader so reverse destruction drops
         * every mapped span consumer before the backend-thread guard promptly
         * forgets, unmaps and closes its PostgreSQL ResourceOwner lease. */
        std::optional<bioseq::storage::indexing::mapped_minimizer_fragment_lease_guard>
            persisted_fragment_mapping_guard{};
        std::optional<bioseq::indexing::prepared_minimizer_fragment_reader>
            persisted_fragment_reader{};
        /* Captured once when the persisted reader is opened.  Production call
         * sites must pass this explicitly: compact_h32_direct is fail-closed and
         * must never silently take the default grouped-SoA control-scan route. */
        bioseq::indexing::minimizer_fragment_lookup_route persisted_fragment_lookup_route =
            bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans;
        /* Minimizer-sequence grouped-SoA retains its sequence-wide query-selected image.
         * It is separate from the minimizer-fragment mmap reader above: minimizer-sequence consumes
         * the authenticated PostgreSQL payload scan and must never borrow the
         * minimizer-fragment-only sidecar or fragment-native ownership validation. */
        std::optional<bioseq::indexing::prepared_grouped_soa_query_reader>
            persisted_grouped_soa_reader{};
        std::optional<bioseq::indexing::hash_major_minimizer_selective_reader>
            persisted_hash_major_reader{};
        ::bioseq::pipeline_stats::target_index_source target_index_source =
            ::bioseq::pipeline_stats::target_index_source::none;
        std::uint64_t key_set_checksum = 0;
        std::uint64_t target_index_builds = 0;
        std::uint64_t target_index_reuses = 0;
        std::uint64_t duplicate_build_attempts = 0;
        std::uint64_t persisted_reader_loads = 0;
        std::uint64_t persisted_reader_queries = 0;
        std::uint64_t persisted_reader_load_host_wall_ns = 0;
        std::uint64_t persisted_generation_id = 0;
        /* Correctness authority for hash-major. The diagnostic u64 field below
         * is retained only for the existing pipeline stats ABI and must never
         * be used to authenticate a hash-major source. */
        std::array<std::byte, 16> persisted_hash_major_source_identity{};
        bool persisted_hash_major_source_identity_present = false;
        std::uint64_t persisted_source_identity = 0;
        bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
        bool build_set_checksum_present = false;
        std::uint64_t build_set_checksum = 0;
        /* Compact-direct full-batch preparation already constructs the exact
         * sorted unique query set needed by the reader.  While this is true the
         * prepared/build checksum is deliberately unpublished; the
         * backend thread must resolve it exactly once before worker fanout or
         * materialize the admitted slow fallback before row execution. */
        bool compact_direct_query_set_validation_pending = false;

        /**
         * @brief Finds a retained sketch by query ordinal.
         * @return The borrowed sketch or null when it was not retained.
         */
        [[nodiscard]] const cpu_prepared_query_sketch*
        find_retained_query_sketch(const std::uint64_t query_ordinal) const noexcept {
            const auto retained_sketch = std::lower_bound(
                retained_query_sketches.begin(),
                retained_query_sketches.end(),
                query_ordinal,
                [](const cpu_prepared_query_sketch& sketch, const std::uint64_t ordinal) noexcept {
                    return sketch.query_ordinal < ordinal;
                });
            return retained_sketch != retained_query_sketches.end() &&
                           retained_sketch->query_ordinal == query_ordinal
                       ? &*retained_sketch
                       : nullptr;
        }
    };

    /**
     * @brief Owns CPU preparation shared by all query waves for one target row.
     *
     * One object per physical target row survives every 8192-query wave
     * in tuple_dispatch_buffer and may contain one parameter group per effective
     * (k,w,max_occ) shape.  GPU locator execution never consumes this object. */
    struct cpu_prepared_target {
        std::int64_t target_rowno = -1;
        std::uint64_t target_bp_size = 0;
        std::uint64_t query_count = 0;
        /* CPU preparation runs beneath the same public Highway selection
         * boundary as ordinary batch execution.  The selected table has static
         * storage duration; retaining its neutral pointer and exact plan here
         * lets every later query wave prove that it is reusing, rather than
         * reconstructing, that target binding. */
        const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;
        ::bioseq::simd_dispatch::highway_dispatch_plan highway_dispatch{};
        std::int64_t highway_executed_target = 0;
        std::vector<cpu_prepared_index_group> groups{};
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
        std::optional<bioseq::parallelism::memory_permit> query_union_permit{};
        std::uint64_t query_sketch_cache_admission_bytes = 0;
        std::optional<bioseq::parallelism::memory_permit> query_sketch_cache_permit{};

        cpu_prepared_target() = default;
        cpu_prepared_target(const cpu_prepared_target&) = delete;
        cpu_prepared_target& operator=(const cpu_prepared_target&) = delete;
        cpu_prepared_target(cpu_prepared_target&&) noexcept = default;
        cpu_prepared_target& operator=(cpu_prepared_target&&) noexcept = default;

        /**
         * @brief Compares the complete Highway dispatch identity retained by a target.
         * @param[in] lhs First dispatch plan.
         * @param[in] lhs_target Executed target for the first plan.
         * @param[in] rhs Second dispatch plan.
         * @param[in] rhs_target Executed target for the second plan.
         * @return True when both plans and executed targets are identical.
         */
        [[nodiscard]] static constexpr bool
        same_highway_dispatch_identity(const ::bioseq::simd_dispatch::highway_dispatch_plan& lhs,
                                       const std::int64_t lhs_target,
                                       const ::bioseq::simd_dispatch::highway_dispatch_plan& rhs,
                                       const std::int64_t rhs_target) noexcept {
            return lhs.compiled_targets == rhs.compiled_targets &&
                   lhs.cpu_supported_targets == rhs.cpu_supported_targets &&
                   lhs.runnable_targets == rhs.runnable_targets &&
                   lhs.planned_target == rhs.planned_target && lhs_target == rhs_target;
        }

        /**
         * @brief Checks whether this target retains the dispatch selected by parameters.
         * @param[in] params Parameters carrying the selected Highway table and plan.
         * @return True when the retained binding matches every dispatch field.
         */
        [[nodiscard]] bool
        highway_binding_matches(const minimap2_parameters& params) const noexcept {
            return highway_kernels != nullptr && highway_kernels == params.highway_kernels &&
                   same_highway_dispatch_identity(highway_dispatch,
                                                  highway_executed_target,
                                                  params.highway_dispatch,
                                                  params.highway_executed_target);
        }

        /**
         * @brief Binds a valid Highway dispatch or validates the existing binding.
         * @param[in] params Parameters carrying the selected Highway table and plan.
         * @return True when a valid binding was installed or already matched.
         */
        [[nodiscard]] bool bind_highway_dispatch_once(const minimap2_parameters& params) noexcept {
            const auto& plan = params.highway_dispatch;
            if (params.highway_kernels == nullptr || plan.compiled_targets == 0 ||
                plan.cpu_supported_targets == 0 ||
                plan.runnable_targets != (plan.compiled_targets & plan.cpu_supported_targets) ||
                params.highway_executed_target <= 0 ||
                !std::has_single_bit(static_cast<std::uint64_t>(params.highway_executed_target)) ||
                (plan.runnable_targets & params.highway_executed_target) == 0 ||
                plan.planned_target != params.highway_executed_target) {
                return false;
            }
            const bool empty = highway_kernels == nullptr &&
                               highway_dispatch.compiled_targets == 0 &&
                               highway_dispatch.cpu_supported_targets == 0 &&
                               highway_dispatch.runnable_targets == 0 &&
                               highway_dispatch.planned_target == 0 && highway_executed_target == 0;
            if (!empty)
                return highway_binding_matches(params);
            highway_kernels = params.highway_kernels;
            highway_dispatch = params.highway_dispatch;
            highway_executed_target = params.highway_executed_target;
            return true;
        }

        /** @brief Releases query-union storage before returning its admission permits. */
        void release_query_union_state_noexcept() noexcept {
            std::vector<cpu_prepared_index_group>{}.swap(groups);
            query_union_admission_bytes = 0;
            query_union_permit.reset();
            query_sketch_cache_admission_bytes = 0;
            query_sketch_cache_permit.reset();
        }

        /**
         * @brief Releases a group's native sketch union and shrinks target admission.
         * @param[in,out] group Prepared group whose native union is no longer needed.
         * @return True when ownership and admission were consistent and released.
         */
        [[nodiscard]] bool
        release_group_query_hash_union_noexcept(cpu_prepared_index_group& group) noexcept {
            const std::uint64_t released_bytes = bioseq::common::saturating_mul(
                group.query_hash_union.capacity(),
                static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            if (released_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                !query_union_permit.has_value() || query_union_admission_bytes < released_bytes ||
                !group.canonical_query_hash_union_u32.empty() ||
                group.canonical_query_hash_union_u32.capacity() != 0u) {
                return false;
            }
            // The native persisted reader owns its canonical hash image now.
            // Deallocate the target-wide sketch union before shrinking the
            // aggregate permit so memory authority never trails ownership.
            std::vector<minimap_sketch_data>{}.swap(group.query_hash_union);
            query_union_admission_bytes -= released_bytes;
            query_union_permit->shrink_to(query_union_admission_bytes);
            return true;
        }
        ~cpu_prepared_target() noexcept {
            release_query_union_state_noexcept();
        }

        /**
         * @brief Finds a prepared group with the requested minimizer parameters.
         * @return The matching borrowed group or null when no group matches.
         */
        [[nodiscard]] cpu_prepared_index_group*
        find_group(std::int32_t kmer_size,
                   std::int32_t window_size,
                   std::int32_t max_occ,
                   minimizer_sketch_backend sketch_backend) noexcept;

        /**
         * @brief Checks whether every compact-direct query-set receipt is resolved.
         * @return True when no prepared group awaits validation.
         */
        [[nodiscard]] bool query_set_validation_ready() const noexcept {
            return std::ranges::none_of(groups, [](const cpu_prepared_index_group& group) noexcept {
                return group.compact_direct_query_set_validation_pending;
            });
        }
    };

    /**
     * @brief Retains one authenticated grouped-SoA generation for a GPU target.
     *
     * The resident owns the three Vulkan buffers for the complete batch join
     * lifecycle. `resident_view` contains stable pointers into that object and
     * has no independent publication or cache authority.
     */
    struct gpu_prepared_grouped_soa_target {
        std::int64_t target_rowno = -1;
        std::uint64_t target_bp_size = 0;
        std::uint64_t query_count = 0;
        std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target> resident{};
        bioseq::gpu::vulkan::gpu_grouped_soa_resident_view resident_view{};
        bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
        std::uint64_t dispatched_query_count = 0;
        std::uint64_t dispatch_waves = 0;
        bool persisted_read_stats_published = false;

        gpu_prepared_grouped_soa_target() = default;
        gpu_prepared_grouped_soa_target(const gpu_prepared_grouped_soa_target&) = delete;
        gpu_prepared_grouped_soa_target& operator=(const gpu_prepared_grouped_soa_target&) = delete;
        gpu_prepared_grouped_soa_target(gpu_prepared_grouped_soa_target&&) noexcept = default;
        gpu_prepared_grouped_soa_target&
        operator=(gpu_prepared_grouped_soa_target&&) noexcept = default;

        /**
         * @brief Validates retained ownership, buffer views, and source identity.
         * @return True when the target is ready for grouped-SoA GPU dispatch.
         */
        [[nodiscard]] bool ready() const noexcept {
            return target_rowno >= 0 && target_bp_size != 0 && resident != nullptr &&
                   resident->validated() && resident_view.descriptors == &resident->descriptors() &&
                   resident_view.controls == &resident->controls() &&
                   resident_view.positions == &resident->positions() &&
                   resident_view.descriptor_bytes == resident->descriptor_bytes() &&
                   resident_view.control_bytes == resident->control_bytes() &&
                   resident_view.position_bytes == resident->position_bytes() &&
                   resident_view.position_count == resident->position_count() &&
                   resident_view.max_occ_cap ==
                       bioseq::indexing::k_grouped_soa_default_max_occurrence_cap &&
                   resident_view.partition_count == resident->descriptor_count() &&
                   resident_view.abi_revision == resident->identity().gpu_abi_revision &&
                   resident_view.device_uuid == resident->identity().device_uuid &&
                   resident_view.generation_id == resident->identity().generation_id &&
                   resident_view.source_identity == resident->identity().source_identity &&
                   resident_view.cold_load_identity == resident->identity().cold_load_identity;
        }
    };

    /**
     * @brief Exercises the deallocate-before-permit reprepare contract.
     *
     * It deliberately populates one cpu_prepared_target object twice instead of
     * constructing a later scan object, so stale outer or nested capacity is
     * observable.  No PostgreSQL state is touched.
     *
     * @return True when move, validation, release, and reprepare checks pass.
     */
    [[nodiscard]] static bool cpu_prepared_target_same_object_reprepare_probe() {
        /* The same existing SQL probe also guards the hash-major borrowed-view
         * lifetime: moving the target must transfer the group allocation
         * without moving or duplicating its canonical u32 image. */
        {
            cpu_prepared_target source;
            source.groups.emplace_back();
            auto& canonical = source.groups.front().canonical_query_hash_union_u32;
            canonical = {3u, 7u, 11u};
            const std::uint32_t* const borrowed = canonical.data();
            const bioseq::indexing::canonical_query_hash_view view{
                .values32 = std::span<const std::uint32_t>{canonical},
                .width_bytes = sizeof(std::uint32_t),
                .storage_width_bytes = sizeof(std::uint32_t),
            };
            cpu_prepared_target moved{std::move(source)};
            if (moved.groups.size() != 1u || !moved.groups.front().query_hash_union.empty() ||
                moved.groups.front().query_hash_union.capacity() != 0u ||
                moved.groups.front().canonical_query_hash_union_u32.data() != borrowed ||
                view.size() != 3u || view.at(0u) != 3u || view.at(1u) != 7u || view.at(2u) != 11u) {
                return false;
            }
        }
        /* The retained-sketch receipt is fail-closed, one-shot, and leaves no
         * canonical owner behind.  Exercise both direct publication and the
         * admitted sort/dedup fallback through the existing SQL seam. */
        {
            cpu_prepared_target validation_target;
            validation_target.unions_ready = true;
            validation_target.query_count = 0u;
            validation_target.key_set_checksum =
                bioseq::indexing::k_compact_direct_query_set_fnv_offset;
            validation_target.groups.emplace_back();
            auto& group = validation_target.groups.front();
            group.kmer_size = 15;
            group.window_size = 10;
            group.max_occ = 500;
            group.sketch_backend = minimizer_sketch_backend::scalar;
            group.compact_direct_query_set_validation_pending = true;
            const bioseq::indexing::compact_direct_query_set_stats query_set_stats{
                .query_count = 0u,
                .query_occurrence_count = 0u,
                .unique_hash_count = 0u,
                .group_key_set_checksum =
                    bioseq::indexing::compact_direct_query_set_checksum_prefix(15u, 10u, 500u),
                .present = true,
            };
            auto rejected = query_set_stats;
            rejected.query_count = 1u;
            if (validation_target.query_set_validation_ready() ||
                group.canonical_query_hash_union_u32.capacity() != 0u ||
                publish_compact_direct_query_set_stats(validation_target, group, rejected) ||
                !group.compact_direct_query_set_validation_pending ||
                !publish_compact_direct_query_set_stats(
                    validation_target, group, query_set_stats) ||
                publish_compact_direct_query_set_stats(validation_target, group, query_set_stats) ||
                !validation_target.query_set_validation_ready() ||
                group.canonical_query_hash_union_u32.capacity() != 0u ||
                validation_target.total_unique_keys != 0u ||
                group.key_set_checksum != query_set_stats.group_key_set_checksum ||
                !group.build_set_checksum_present ||
                group.build_set_checksum != group.key_set_checksum) {
                return false;
            }

            cpu_prepared_target fallback_target;
            fallback_target.unions_ready = true;
            fallback_target.query_count = 0u;
            fallback_target.key_set_checksum =
                bioseq::indexing::k_compact_direct_query_set_fnv_offset;
            fallback_target.groups.emplace_back();
            auto& fallback_group = fallback_target.groups.front();
            fallback_group.kmer_size = 15;
            fallback_group.window_size = 10;
            fallback_group.max_occ = 500;
            fallback_group.sketch_backend = minimizer_sketch_backend::scalar;
            fallback_group.compact_direct_query_set_validation_pending = true;
            try {
                finalize_pending_compact_direct_query_set_validation_fallback(fallback_target);
            } catch (...) {
                return false;
            }
            if (!fallback_target.query_set_validation_ready() ||
                fallback_group.canonical_query_hash_union_u32.capacity() != 0u ||
                fallback_target.total_unique_keys != 0u ||
                fallback_group.key_set_checksum != query_set_stats.group_key_set_checksum ||
                fallback_target.key_set_checksum != validation_target.key_set_checksum) {
                return false;
            }
        }
        bioseq::parallelism::memory_admission_controller admission;
        cpu_prepared_target target;
        auto populate = [&admission, &target](const std::size_t sketch_capacity) -> bool {
            target.release_query_union_state_noexcept();
            auto permit = admission.try_reserve(0u);
            if (!permit)
                return false;
            target.query_union_permit.emplace(std::move(*permit));

            const std::uint64_t outer_headroom = bioseq::common::saturating_mul(
                sizeof(cpu_prepared_index_group),
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
            if (outer_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
                !target.query_union_permit->grow_to(outer_headroom)) {
                return false;
            }
            target.groups.reserve(1u);
            const std::uint64_t outer_bytes = bioseq::common::saturating_mul(
                target.groups.capacity(), sizeof(cpu_prepared_index_group));
            if (outer_bytes > outer_headroom)
                return false;
            target.query_union_admission_bytes = outer_bytes;
            target.query_union_permit->shrink_to(outer_bytes);
            target.groups.emplace_back();

            const std::uint64_t inner_headroom = bioseq::common::saturating_mul(
                bioseq::common::saturating_mul(sketch_capacity, sizeof(minimap_sketch_data)),
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
            const std::uint64_t peak = bioseq::common::saturating_add(outer_bytes, inner_headroom);
            if (inner_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
                peak == (std::numeric_limits<std::uint64_t>::max)() ||
                !target.query_union_permit->grow_to(peak)) {
                return false;
            }
            target.groups.front().query_hash_union.reserve(sketch_capacity);
            const std::uint64_t inner_bytes = bioseq::common::saturating_mul(
                target.groups.front().query_hash_union.capacity(), sizeof(minimap_sketch_data));
            const std::uint64_t retained = bioseq::common::saturating_add(outer_bytes, inner_bytes);
            if (inner_bytes > inner_headroom ||
                retained == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            target.query_union_admission_bytes = retained;
            target.query_union_permit->shrink_to(retained);
            return admission.reserved_bytes() == retained;
        };

        if (!populate(8193u))
            return false;
        const std::size_t oversized_capacity = target.groups.front().query_hash_union.capacity();
        const std::uint64_t retained_outer_bytes = bioseq::common::saturating_mul(
            target.groups.capacity(), static_cast<std::uint64_t>(sizeof(cpu_prepared_index_group)));
        if (!target.release_group_query_hash_union_noexcept(target.groups.front()) ||
            !target.groups.front().query_hash_union.empty() ||
            target.groups.front().query_hash_union.capacity() != 0u ||
            target.query_union_admission_bytes != retained_outer_bytes ||
            target.query_union_permit->bytes() != retained_outer_bytes ||
            admission.reserved_bytes() != retained_outer_bytes) {
            return false;
        }
        target.release_query_union_state_noexcept();
        if (!target.groups.empty() || target.groups.capacity() != 0u ||
            target.query_union_permit.has_value() || target.query_union_admission_bytes != 0u ||
            admission.reserved_bytes() != 0u) {
            return false;
        }
        if (!populate(1u))
            return false;
        const bool smaller_reprepare =
            target.groups.size() == 1u &&
            target.groups.front().query_hash_union.capacity() < oversized_capacity;
        target.release_query_union_state_noexcept();
        return smaller_reprepare && target.groups.capacity() == 0u &&
               !target.query_union_permit.has_value() && admission.reserved_bytes() == 0u;
    }

    /**
     * @brief Checks that exactly one persisted native reader was selected.
     * @return True when exactly one reader flag is set.
     */
    [[nodiscard]] static constexpr bool exactly_one_native_persisted_reader(
        const bool fragment, const bool grouped_soa, const bool hash_major) noexcept {
        return static_cast<unsigned>(fragment) + static_cast<unsigned>(grouped_soa) +
                   static_cast<unsigned>(hash_major) ==
               1u;
    }

    /**
     * @brief Checks whether grouped-SoA may replace hash-major before stream I/O.
     * @return True only before I/O when the head is absent or inapplicable.
     */
    [[nodiscard]] static constexpr bool
    hash_major_grouped_soa_fallback_allowed(const bool head_absent,
                                            const bool profile_applicable,
                                            const bool stream_io_started) noexcept {
        return !stream_io_started && (head_absent || !profile_applicable);
    }

    /**
     * @brief Derives length-adjusted minimap2 parameters for one query pair.
     * @param[in] base Base parameters selected for the statement.
     * @param[in] min_bp Shorter sequence length in base pairs.
     * @return Effective parameters for the pair.
     */
    [[nodiscard]] static minimap2_parameters
    effective_params_for_pair(const minimap2_parameters& base, std::size_t min_bp) noexcept;

    /**
     * @brief Prepares admitted CPU query unions shared by every target wave.
     *
     * Flat and recipe-backed queries use the supplied typmod operations and
     * publish only after all retained storage has matching admission authority.
     */
    void prepare_cpu_shared_query_unions(
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint64_t target_bp_size,
        std::span<const bioseq::pg_types::seq* const> query_headers,
        std::span<const Datum> query_recipe_datums,
        std::span<const pg_sequence_descriptor> query_recipe_descriptors,
        pg_sequence_read_session* query_read_session,
        const bioseq::storage::sequence_payload_allocation_observer* query_payload_observer,
        std::span<const std::uint64_t> query_ordinals,
        const minimap2_parameters& base_params,
        bool include_persisted_parameter_group,
        cpu_query_union_storage union_storage,
        const cpu_preparation_typmod_operations& typmod_ops,
        cpu_prepared_target& prepared_target);

    /**
     * @brief Opens and validates one persisted native CPU target representation.
     *
     * The selected reader remains bound to the executor snapshot and the prepared
     * query unions retained by `prepared_target`.
     */
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
        const bioseq::storage::sequence_payload_allocation_observer* query_payload_observer,
        std::span<const std::uint64_t> query_ordinals,
        const minimap2_parameters& base_params,
        const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
        const cpu_preparation_typmod_operations& typmod_ops,
        cpu_prepared_target& prepared_target);

    /**
     * @brief Publishes one authenticated compact-direct query-set receipt.
     * @return True when the pending group accepted the complete receipt.
     */
    [[nodiscard]] static bool publish_compact_direct_query_set_stats(
        cpu_prepared_target& prepared_target,
        cpu_prepared_index_group& group,
        const bioseq::indexing::compact_direct_query_set_stats& query_set_stats) noexcept;

    /**
     * @brief Resolves pending compact-direct validation from admitted query unions.
     */
    static void finalize_pending_compact_direct_query_set_validation_fallback(
        cpu_prepared_target& prepared_target);

    /**
     * @brief Opens one authenticated persisted grouped-SoA target for Vulkan use.
     */
    void prepare_gpu_persisted_grouped_soa_target(
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
        const minimap2_parameters& params,
        const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
        gpu_prepared_grouped_soa_target& prepared_target);

    /**
     * @brief Carries statement-owned PostgreSQL resources into per-row dispatch.
     *
     * When present, locator targets remain source-backed while query literals use
     * resident facades. A null context selects the in-memory baseline stage.
     */
    struct scanner_pg_context {
        /**
         * @brief Describes one synchronous cold-preparation callback.
         *
         * The scheduler root installs it and `process_sequences` consumes it after
         * selecting parameters but before profiling or typed execution.
         */
        struct highway_preparation_hook final {
            /** @brief Function signature for statement-root cold preparation. */
            using callback_function = void (*)(void*,
                                               scanner_pg_context&,
                                               const minimap2_parameters&,
                                               const cpu_preparation_typmod_operations&);

            void* state = nullptr;
            callback_function function = nullptr;

            /** @return True when neither callback component is installed. */
            [[nodiscard]] constexpr bool empty() const noexcept {
                return state == nullptr && function == nullptr;
            }

            /** @return True when both callback components are installed. */
            [[nodiscard]] constexpr bool valid() const noexcept {
                return state != nullptr && function != nullptr;
            }
        };

        highway_preparation_hook highway_preparation{};
        /* Optional statement-root async ownership observer.  It is generic at
         * the pipeline boundary; the PostgreSQL trace owner supplies the
         * concrete enrollment/state-machine adapter. */
        bioseq::parallelism::tracked_job_observer async_job_observer{};
        /* Stable for the complete process_sequences call. Views copy only a
         * pointer to these records; every tracked task joins/drains before
         * scanner_pg_context leaves its owning dispatch frame. */
        bioseq::storage::sequence_payload_allocation_observer mmsearch_target_payload_observer{};
        bioseq::storage::sequence_payload_allocation_observer mmsearch_query_payload_observer{};
        Oid db_oid = InvalidOid;
        Oid tablespace_oid = InvalidOid;
        Oid main_relfilenode = InvalidOid;
        std::int16_t mmsearch_seq_attno = 0;
        std::int16_t mmsearch_query_attno = 0;
        std::uint8_t query_alphabet_code = 0;
        std::uint8_t query_storage_unit_byte_width = 0;
        bioseq::storage::seq_caches* caches = nullptr;
        MemoryContext parent_mctx = nullptr;
        /* Wave-owned and borrowed. The dispatch buffer closes it only after
         * process_sequences and every synchronous GPU fence have returned. */
        pg_sequence_read_session* read_session = nullptr;
        // Borrowed statement snapshot owned by the executor/admin caller.
        // The facade adapter validates registered MVCC state and never
        // changes its registration count.
        Snapshot snapshot = nullptr;
        // Borrowed from the bioseq scan that owns this dispatch. Locator-backed
        // targets must be promoted through this exact statement provider;
        // scanner_pg_context never closes or retains it independently.
        bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
        // Borrowed statement-root sink. It is required only by the accepted
        // persisted grouped-SoA Vulkan route and never owns through hot stats.
        ::bioseq::pipeline_stats::persisted_gpu_operation_accumulator*
            persisted_gpu_operation_stats_sink = nullptr;
        // Per-row optional locator. nullopt means "use the inline bioseq::pg_types::seq at
        // target_hdrs[i]" — the target then uses an in-memory facade.
        std::array<std::optional<bioseq::storage::sequence_locator>, dispatch_batch_capacity>
            target_locators{};
        // Same-relation per-row query locators. These use the target scan's
        // exact provider but retain their own physical attno and storage width.
        std::array<std::optional<bioseq::storage::sequence_locator>, dispatch_batch_capacity>
            query_locators{};
        /* Borrowed, batch-arena-owned expanded wrappers captured by
         * tuple_dispatch_buffer.  A nonzero Datum is always paired with its
         * validated descriptor.  Typed dispatch adapts these through the same
         * alphabet-only lazy_sequence_view ABI as locator and flat
         * inputs; it never owns or flattens the wrappers on CPU/SIMD paths. */
        std::array<Datum, dispatch_batch_capacity> target_recipe_datums{};
        std::array<Datum, dispatch_batch_capacity> query_recipe_datums{};
        std::array<pg_sequence_descriptor, dispatch_batch_capacity> target_recipe_descriptors{};
        std::array<pg_sequence_descriptor, dispatch_batch_capacity> query_recipe_descriptors{};
        // Per-row columnar rowno (== index query_key) and the
        // user relation oid, so the stored per-fragment index can be assembled.
        std::array<std::int64_t, dispatch_batch_capacity> target_rownos{};
        std::array<std::uint64_t, dispatch_batch_capacity> query_ordinals{};
        bool explicit_query_ordinals = false;
        bool shared_target_batch = false;
        bool record_target_operational_outcomes = true;
        cpu_prepared_target* cpu_prepared_target = nullptr;
        gpu_prepared_grouped_soa_target* gpu_prepared_grouped_soa_target = nullptr;
        // Explicit per-row outcome written by the scanner helper. Match
        // production and index-source accounting must never infer one another.
        std::array<::bioseq::pipeline_stats::target_index_source, dispatch_batch_capacity>
            target_index_sources{};
        std::array<std::uint64_t, dispatch_batch_capacity> persisted_generation_ids{};
        std::array<std::uint64_t, dispatch_batch_capacity> persisted_source_identities{};
        Oid user_relid = InvalidOid;
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
        bool read_index_active = false;
        bool index_async_persist_active = false;
        bool recompute_async_persist_active = false;
        // The table's ACTIVE read index for the target
        // column. The source is retained separately so a configured catalog row
        // can never be laundered into an indistinguishable no-row default.
        std::string active_index;
        bioseq::storage::indexing::active_read_policy_source active_index_policy_source =
            bioseq::storage::indexing::active_read_policy_source::unresolved;
    };
    /**
     * @brief Runs the minimap2 pipeline across a fixed scheduler batch.
     *
     * For each target row, the loop assembles the table's active per-sequence
     * minimizer index from chunked storage when enabled and present. Otherwise it
     * recomputes the index. Both strategies can be mixed within one batch.
     *
     * `scanner_pg_ctx` carries the batch identity and the optional locator decoded
     * at submit_scan time. Unless the in-memory scanner is forced, targets with a
     * locator use a source-backed facade and inline targets use an in-memory
     * facade. Queries use an in-memory facade over the batch literal. Algorithms
     * see only the alphabet-specialized erased view.
     *
     * `target_hdrs` and `query_ids` are full-size
     * `dispatch_batch_capacity`-element arrays passed by const reference. The call
     * does not copy them. The result map is returned by value.
     *
     * @return Match rows grouped by scheduler query identifier.
     */
    [[nodiscard]] std::map<int32_t, std::vector<match_result>> process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const bioseq::pg_types::seq* query_hdr,
        const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
        const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
        const std::array<int32_t, dispatch_batch_capacity>& query_ids,
        const minimap2_parameters& base_params,
        batch_invocation_role invocation_role,
        std::uint64_t broadcast_query_ordinal,
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        scanner_pg_context* scanner_pg_ctx = nullptr);
};

static_assert(batch_processor::exactly_one_native_persisted_reader(true, false, false));
static_assert(batch_processor::exactly_one_native_persisted_reader(false, true, false));
static_assert(batch_processor::exactly_one_native_persisted_reader(false, false, true));
static_assert(!batch_processor::exactly_one_native_persisted_reader(false, false, false));
static_assert(!batch_processor::exactly_one_native_persisted_reader(true, true, false));
static_assert(!batch_processor::exactly_one_native_persisted_reader(true, false, true));
static_assert(!batch_processor::exactly_one_native_persisted_reader(false, true, true));
static_assert(!batch_processor::exactly_one_native_persisted_reader(true, true, true));
static_assert(batch_processor::hash_major_grouped_soa_fallback_allowed(true, false, false));
static_assert(batch_processor::hash_major_grouped_soa_fallback_allowed(false, false, false));
static_assert(!batch_processor::hash_major_grouped_soa_fallback_allowed(false, true, false));
static_assert(!batch_processor::hash_major_grouped_soa_fallback_allowed(true, false, true));
static_assert(!batch_processor::hash_major_grouped_soa_fallback_allowed(false, false, true));

} // namespace bioseq::simd
