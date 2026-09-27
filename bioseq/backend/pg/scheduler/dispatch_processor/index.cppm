/**
 * @file
 * @brief Prepare shared target indexes and dispatch sequence-alignment batches.
 */
/*
 * Keep alignment templates outside the tuple-buffer interface. The processor
 * selects the implementation for the sequence alphabet and storage width.
 *
 * active_index chooses the stored or temporary minimizer layout. The read
 * requirement separately controls whether a stored index is allowed, forbidden,
 * or required. Missing required indexes raise an  error. Background persistence
 * has its own settings and irite policy. Memory permits bound the indexes
 * and query sketches kept across batches.
 */
module;

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "postgres_ext.h"

// Declare Datum for the selected PostgreSQL server version without pulling
// server headers into this interface. PostgreSQL 19 uses uint64_t.
#if POSTGRESQL_VERSION_MAJOR >= 19
typedef uint64_t Datum;
#else
typedef uintptr_t Datum;
#endif
typedef struct MemoryContextData* MemoryContext;
typedef struct SnapshotData* Snapshot;
}

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor;
export import bioseq.postgres.scheduler.dispatch_processor.hash_major_diagnostic_identity;

import bioseq.common.checked_arithmetic;
export import bioseq.common.seqan_adapter;
export import bioseq.highway.kernel_table_contract;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.indexing.minimizer_sequence;
#endif
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.active_read_policy;
import bioseq.postgres.storage.indexing.executor_read_context;
import bioseq.postgres.storage.indexing.minimizer_fragment_mapping_lease;
import bioseq.postgres.storage.indexing.minimizer_read_stats;
export import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.runtime_contract;
export import bioseq.pipeline.pipe;
export import bioseq.pipeline.minimap_port.match_result_contract;
export import bioseq.pipeline.minimap_port;
export import bioseq.pipeline.minimizer_execution_contract;
export import bioseq.pipeline.stats;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.highway_minimizer_contract;
export import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
export import bioseq.storage.lazy_sequence_view;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner.source_provider_contract;
export import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
namespace bioseq::simd {

export using ::bioseq::minimizers::minimap_sketch_data;

/** @brief Maximum number of rows processed by one scheduler dispatch batch. */
export constexpr size_t dispatch_batch_capacity = 8192;

/** @brief Distinguish the primary alignment from an extra mmsearch filter */
export enum class batch_invocation_role : std::uint8_t {
    primary,
    extra_query,
};

/**
 * @brief Run alignment batches for one alphabet and storage width.
 * Inputs can be flat sequences, storage locators, or saved slices. The
 * scanner context supplies PostgreSQL readers and optional prepared targets.
 * Shared targets keep indexes and query sketches alive  across batches.
 * Execution counters use the, common pipeline statistics types.
 */
export class batch_processor {
  public:
    /** @brief Choose whether preparation saves minimizer records, hashes, or per-query sketches. */
    enum class cpu_query_union_storage : std::uint8_t {
        minimizer_sketch,
        canonical_hashes,
        canonical_hashes_sequential_fallback,
        retained_sketch_only,
    };

    /** @brief Own one query's minimizer records and the permit covering their memory. */
    struct cpu_prepared_query_sketch {
        std::uint64_t query_ordinal = 0;
        std::uint64_t query_bp_size = 0;
        /* Each worker initially owns a permit for its query. The backend merges
         * those permits before saving the complete query set. Declare the permit
         * first so record storage is destroyed before its reservation is released. */
        std::optional<bioseq::parallelism::memory_permit> parallel_preparation_permit{};
        ::bioseq::minimizers::minimizer_physical_batch_owner minimizers{};
        pipe::minimizer_range_execution_stats execution_stats{};

        cpu_prepared_query_sketch() = default;
        cpu_prepared_query_sketch(const cpu_prepared_query_sketch&) = delete;
        cpu_prepared_query_sketch& operator=(const cpu_prepared_query_sketch&) = delete;

        cpu_prepared_query_sketch(
            const std::uint64_t ordinal,
            const std::uint64_t bp_size,
            std::optional<bioseq::parallelism::memory_permit> permit,
            ::bioseq::minimizers::minimizer_physical_batch_owner physical_minimizers,
            pipe::minimizer_range_execution_stats stats) noexcept
            : query_ordinal(ordinal), query_bp_size(bp_size),
              parallel_preparation_permit(std::move(permit)),
              minimizers(std::move(physical_minimizers)), execution_stats(std::move(stats)) {}

        cpu_prepared_query_sketch(cpu_prepared_query_sketch&& other) noexcept
            : query_ordinal(std::exchange(other.query_ordinal, 0u)),
              query_bp_size(std::exchange(other.query_bp_size, 0u)),
              parallel_preparation_permit(std::move(other.parallel_preparation_permit)),
              minimizers(std::move(other.minimizers)),
              execution_stats(std::exchange(
                  other.execution_stats, pipe::minimizer_range_execution_stats{})) {
            other.parallel_preparation_permit.reset();
        }

        cpu_prepared_query_sketch& operator=(cpu_prepared_query_sketch&& other) noexcept {
            if (this != std::addressof(other)) {
                // Free the old records before releasing the  permit that covers them.
                minimizers.reset();
                parallel_preparation_permit.reset();
                query_ordinal = std::exchange(other.query_ordinal, 0u);
                query_bp_size = std::exchange(other.query_bp_size, 0u);
                parallel_preparation_permit =
                    std::move(other.parallel_preparation_permit);
                minimizers = std::move(other.minimizers);
                execution_stats = std::exchange(
                    other.execution_stats, pipe::minimizer_range_execution_stats{});
                other.parallel_preparation_permit.reset();
            }
            return *this;
        }
    };

    /** @brief Preparation state, including pending work and the reason a query failed. */
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

    /** @brief Append minimizers from a flat query to a shared collection. */
    template <class p_destination>
    using cpu_append_header_function =
        void (*)(const bioseq::pg_types::seq&,
                 std::uint64_t,
                 std::int32_t,
                 std::int32_t,
                 minimizer_sketch_backend,
                 const ::bioseq::minimizers::minimizer_physical_layout_selection&,
                 const ::bioseq::simd_dispatch::highway_kernel_table*,
                 std::vector<p_destination>&,
                 bioseq::parallelism::memory_permit&,
                 std::uint64_t&,
                 std::vector<cpu_prepared_query_sketch>*,
                 bioseq::parallelism::memory_permit*,
                 std::uint64_t*);

    /** @brief Append minimizers from an expanded querg using its sequence read session. */
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
                 const ::bioseq::minimizers::minimizer_physical_layout_selection&,
                 const ::bioseq::simd_dispatch::highway_kernel_table*,
                 std::vector<p_destination>&,
                 bioseq::parallelism::memory_permit&,
                 std::uint64_t&,
                 std::vector<cpu_prepared_query_sketch>*,
                 bioseq::parallelism::memory_permit*,
                 std::uint64_t*);

    /** @brief Prepare one query sketch with its own memory permit. */
    using cpu_prepare_independent_function =
        cpu_preparation_status (*)(const bioseq::pg_types::seq&,
                                   std::uint64_t,
                                   std::int32_t,
                                   std::int32_t,
                                   minimizer_sketch_backend,
                                   const ::bioseq::minimizers::
                                       minimizer_physical_layout_selection&,
                                   const ::bioseq::simd_dispatch::highway_kernel_table*,
                                   cpu_prepared_query_sketch&);

    /** @brief Select a query record format for the bound alphabet and storage width. */
    using cpu_select_query_layout_function =
        ::bioseq::minimizers::minimizer_physical_layout_selection (*)(
            minimizer_algorithm, std::uint64_t, std::int32_t, std::int32_t);

    /** @brief Query-preparation functions compiled for one alphabet and storage width. */
    struct cpu_preparation_typmod_operations final {
        std::uint8_t alphabet_code = 0;
        std::uint8_t storage_unit_byte_width = 0;
        cpu_select_query_layout_function select_query_layout = nullptr;
        cpu_append_header_function<minimap_sketch_data> append_header_sketch = nullptr;
        cpu_append_header_function<std::uint32_t> append_header_u32 = nullptr;
        cpu_append_header_function<std::uint64_t> append_header_u64 = nullptr;
        cpu_append_recipe_function<minimap_sketch_data> append_recipe_sketch = nullptr;
        cpu_append_recipe_function<std::uint32_t> append_recipe_u32 = nullptr;
        cpu_append_recipe_function<std::uint64_t> append_recipe_u64 = nullptr;
        cpu_prepare_independent_function prepare_independent = nullptr;

        /** @return Whether the width is a power of two and  every required function is present */
        [[nodiscard]] constexpr bool complete() const noexcept {
            return std::has_single_bit(storage_unit_byte_width) &&
                   select_query_layout != nullptr && append_header_sketch != nullptr &&
                   append_header_u32 != nullptr && append_header_u64 != nullptr &&
                   append_recipe_sketch != nullptr && append_recipe_u32 != nullptr &&
                   append_recipe_u64 != nullptr &&
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

    /** @brief Queries and a shared target index for one set of minimizer parameters. */
    struct cpu_prepared_index_group {
        std::int32_t kmer_size = 0;
        std::int32_t window_size = 0;
        std::int32_t max_occ = 0;
        minimizer_sketch_backend sketch_backend = minimizer_sketch_backend::scalar;
        ::bioseq::minimizers::minimizer_physical_layout_selection execution_layout{};
        std::vector<minimap_sketch_data> query_hash_union{};
        /* Sorted unique query hashes in their selected width. Some readers borrow
         * these arrays, so declare them before the readers. Use one width at
         * a time, and keep query_hash_union empty while this form is active. */
        std::vector<std::uint32_t> canonical_query_hash_union_u32{};
        std::vector<std::uint64_t> canonical_query_hash_union_u64{};
        std::uint8_t canonical_query_hash_width = 0u;
        /* Keep each query's original generated records alongside its hash union
         * so row execution can reuse the same sketch format. */
        std::vector<cpu_prepared_query_sketch> retained_query_sketches{};
        /* Destroy target_index before returning the memory covered by its permit. */
        std::optional<bioseq::parallelism::memory_permit> target_index_permit{};
        std::shared_ptr<bioseq::indexing::minimizer_hashmap_fragment_index> target_index{};
        /* The MPF reader probes stored fragment indexes or,  their direct-lookup
         * image and applies the query's occurrence limit. */
        /* Keep the mapping alive until the borrowing reader has been destroyed.
         * The mapping owner then releases its PostgreSQL resource lese. */
        std::optional<bioseq::storage::indexing::mapped_minimizer_fragment_lease_guard>
            persisted_fragment_mapping_guard{};
        std::optional<bioseq::indexing::prepared_minimizer_fragment_reader>
            persisted_fragment_reader{};
        /* Keep the lookup route selected when the reader opened. Callers pass it
         * explicitly so direct lookup cannot silently become a control scan. */
        bioseq::indexing::minimizer_fragment_lookup_route persisted_fragment_lookup_route =
            bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans;
        /* The MPS reader owns positions selected from a complete PostgreSQL
         * payload scan. Its storage is separate from the mapped MPF reader. */
        std::optional<bioseq::indexing::prepared_grouped_soa_query_reader>
            persisted_grouped_soa_reader{};
        /* Temporary MPS reader  for this query group. It keeps selected positions
         * alive after the temporary encoded index has been released. */
        std::optional<bioseq::indexing::prepared_grouped_soa_query_reader>
            transient_grouped_soa_reader{};
        std::optional<bioseq::indexing::hash_major_minimizer_selective_reader>
            persisted_hash_major_reader{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        /* Benchmark and test builds save the target producer's execution record
         * beside the reader that consumed its temporary output */
        std::optional<bioseq::indexing::minimizer_sequence_generation_execution_receipt>
            transient_target_generation_receipt{};
#endif
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
        /* Use all 128 bits to check a hash-major source. The 64-bit field below
         * is only a diagnostic token for that layout. */
        std::array<std::byte, 16> persisted_hash_major_source_identity{};
        bool persisted_hash_major_source_identity_present = false;
        std::uint64_t persisted_source_identity = 0;
        bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
        bool build_set_checksum_present = false;
        std::uint64_t build_set_checksum = 0;
        /* Query-set counts and checksums are pending until the complete parallel
         * batch validates them. A smaller execution batch must first  derive
         * them from the saved sketches. */
        bool compact_direct_query_set_validation_pending = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        bool transient_target_generation_stats_published = false;
#endif

        /**
         * @brief Find a saved sketch by query ordinal.
         * @return The borrowed sketch, or null when no sketch has that ordinal.
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

        /** Return the selected hawh width, or zero if the stored arrays disagree with it. */
        [[nodiscard]] std::uint8_t canonical_query_hash_width_bytes() const noexcept {
            const bool valid_u32 = canonical_query_hash_width == sizeof(std::uint32_t) &&
                                   canonical_query_hash_union_u64.capacity() == 0u;
            const bool valid_u64 = canonical_query_hash_width == sizeof(std::uint64_t) &&
                                   canonical_query_hash_union_u32.capacity() == 0u;
            if (valid_u32 == valid_u64) {
                return 0u;
            }
            return canonical_query_hash_width;
        }

        /** Borrow the query hashes with their width and optional storage-profile digest. */
        [[nodiscard]] bioseq::indexing::canonical_query_hash_view
        canonical_query_hash_view(
            const std::array<std::byte, 32>& storage_profile_digest = {}) const noexcept {
            return {
                .values32 = std::span<const std::uint32_t>{canonical_query_hash_union_u32},
                .values64 = std::span<const std::uint64_t>{canonical_query_hash_union_u64},
                .width_bytes = canonical_query_hash_width_bytes(),
                .storage_width_bytes = canonical_query_hash_width_bytes(),
                .storage_profile_digest = storage_profile_digest,
            };
        }
    };

    /**
     * @brief Keep one MPF source and its reusable Vulkan index.
     * Stored targets keep a mapped fragment reader until the GPU index is
     * ready. A temporary target starts with source metadata and builds from
     * the  target sequence. Queries share the completed GPU index.
     */
    struct gpu_prepared_mpf_target {
        pipe::statement_execution_plan_owner execution_plan{};
        std::int64_t target_rowno = -1;
        std::uint64_t target_bp_size = 0;
        std::uint64_t query_count = 0;
        /* Destroy the reader before releasing the mapped file and resource lease. */
        std::optional<bioseq::storage::indexing::mapped_minimizer_fragment_lease_guard>
            persisted_fragment_mapping_guard{};
        std::optional<bioseq::indexing::prepared_minimizer_fragment_reader>
            persisted_fragment_reader{};
        std::shared_ptr<bioseq::indexing::gpu_minimizer_index_cache> resident_cache{};
        std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index> resident{};
        bioseq::indexing::gpu_minimizer_index_cache_key resident_key{};
        bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
        std::uint64_t persisted_generation_id = 0;
        std::uint64_t persisted_source_identity = 0;
        std::uint64_t operation_identity = 0;
        std::uint64_t dispatched_query_count = 0;
        std::uint64_t dispatch_waves = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        /* Vulkan collects target minimizers. The CPU replays temporary runs and
         * builds the sorted dense index, so keep their execution records separate. */
        std::optional<bioseq::indexing::minimizer_sequence_generation_execution_receipt>
            transient_target_spill_receipt{};
        std::optional<bioseq::indexing::minimizer_sequence_generation_execution_receipt>
            transient_target_replay_receipt{};
        /* Record the completed GPU allocation while the resident owns its buffers
         * and, memory permit. Used for stored and temporary nonempty targets. */
        std::optional<bioseq::indexing::gpu_source_direct_materialization_stats>
            resident_materialization_stats{};
#endif
        bool persisted = false;
        bool canonical_empty = false;
        bool persisted_read_stats_published = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        bool target_physical_stats_published = false;
#endif

        gpu_prepared_mpf_target() = default;
        gpu_prepared_mpf_target(const gpu_prepared_mpf_target&) = delete;
        gpu_prepared_mpf_target& operator=(const gpu_prepared_mpf_target&) = delete;
        gpu_prepared_mpf_target(gpu_prepared_mpf_target&&) noexcept = default;
        gpu_prepared_mpf_target& operator=(gpu_prepared_mpf_target&& other) noexcept {
            if (this != std::addressof(other)) {
                // Destroy the current object in reverse member order before moving in
                // its replacement This keeps the mapping alive through reader
                // destruction.
                std::destroy_at(this);
                std::construct_at(this, std::move(other));
            }
            return *this;
        }

        /** @return Whether the  plan, row identity, cache, and stored-index IDs are consistent. */
        [[nodiscard]] bool identity_ready() const noexcept {
            if (!execution_plan || target_rowno < 0 || target_bp_size == 0u ||
                query_count == 0u || operation_identity == 0u || resident_cache == nullptr) {
                return false;
            }
            return persisted
                ? persisted_generation_id != 0u && persisted_source_identity != 0u
                : persisted_generation_id == 0u && persisted_source_identity == 0u;
        }

        /** @return Whether the taget has the source state needed to build its GPU index. */
        [[nodiscard]] bool source_ready() const noexcept {
            if (!identity_ready())
                return false;
            if (!persisted) {
                return !persisted_fragment_mapping_guard && !persisted_fragment_reader;
            }
            return persisted_fragment_mapping_guard.has_value() &&
                   persisted_fragment_reader.has_value() &&
                   persisted_fragment_reader->gpu_source_ready();
        }

        /** @return Whether a complete GPU index or checked empty result matches this request. */
        [[nodiscard]] bool completed_resident_ready() const noexcept {
            if (!identity_ready())
                return false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const bool has_spill = transient_target_spill_receipt.has_value();
            const bool has_replay = transient_target_replay_receipt.has_value();
            const bool has_materialization = resident_materialization_stats.has_value();
            const bool transient_proof = persisted
                ? !has_spill && !has_replay
                : (!has_spill
                       ? canonical_empty && !has_replay
                       : transient_target_spill_receipt->is_spill_valid() &&
                             transient_target_spill_receipt->selected_backend ==
                                 minimizer_sketch_backend::vulkan_segment &&
                             transient_target_spill_receipt->target_length == target_bp_size &&
                             (canonical_empty
                                  ? transient_target_spill_receipt->spilled_event_count == 0u &&
                                        !has_replay
                                  : has_replay &&
                                        transient_target_replay_receipt->is_spill_valid() &&
                                        transient_target_replay_receipt->spill_operation_epoch ==
                                            transient_target_spill_receipt
                                                ->spill_operation_epoch &&
                                        transient_target_replay_receipt
                                                ->spill_storage_binding ==
                                            transient_target_spill_receipt
                                                ->spill_storage_binding &&
                                        transient_target_replay_receipt
                                                ->spilled_event_count ==
                                            transient_target_spill_receipt
                                                ->spilled_event_count &&
                                        transient_target_replay_receipt
                                                ->spill_content_digest ==
                                            transient_target_spill_receipt
                                                ->spill_content_digest &&
                                        transient_target_replay_receipt
                                                ->replay_scan_count == 2u &&
                                        transient_target_replay_receipt
                                                ->replay_merge_pass_count == 2u &&
                                        transient_target_replay_receipt
                                                ->replay_completed_scan_count == 2u &&
                                        transient_target_replay_receipt
                                                ->replay_failed_scan_count == 0u &&
                                        transient_target_replay_receipt
                                                ->replay_illegal_span_transition_count == 0u));
            if (!transient_proof || (canonical_empty == (resident != nullptr)) ||
                (resident != nullptr) != has_materialization)
                return false;
#else
            if (canonical_empty == (resident != nullptr))
                return false;
#endif
            const auto& params = execution_plan->frozen_parameters;
            const bool key_matches_statement =
                resident_key.source_id == 0u &&
                resident_key.caller_identity == operation_identity &&
                resident_key.operation_identity == operation_identity &&
                resident_key.target_len == target_bp_size &&
                resident_key.kmer_size == static_cast<std::uint32_t>(params.kmer_size) &&
                resident_key.window_size == static_cast<std::uint32_t>(params.window_size) &&
                resident_key.source_direct_sorted_dense &&
                resident_key.representation ==
                    bioseq::indexing::gpu_index_representation::source_direct_sorted_dense;
            if (!key_matches_statement)
                return false;
            if (canonical_empty)
                return resident_key.table_size == 0u && resident_key.position_count == 0u;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const auto& physical = *resident_materialization_stats;
            const auto actual_bytes = bioseq::common::checked_add(
                physical.actual_slot_allocation_bytes,
                physical.actual_position_allocation_bytes);
            return resident->target_len == target_bp_size && resident->key == resident_key &&
                   resident_key.operation_identity == operation_identity && actual_bytes &&
                   *actual_bytes == physical.actual_allocation_bytes &&
                   physical.actual_slot_allocation_bytes == resident->slots.allocation_bytes &&
                   physical.actual_position_allocation_bytes ==
                       resident->positions.allocation_bytes &&
                   physical.fully_touched_bytes == physical.actual_allocation_bytes &&
                   physical.source_checksum == physical.mapped_checksum &&
                   physical.source_checksum == resident->source_stream_checksum &&
                   physical.mapped_checksum == resident->mapped_stream_checksum &&
                   physical.shape.minimizer_count == resident->position_count &&
                   physical.shape.distinct_count == resident->unique_key_count &&
                   physical.complete_source_traversals ==
                       resident->complete_source_traversals &&
                   physical.complete_mapped_traversals ==
                       resident->complete_mapped_traversals;
#else
            return resident->target_len == target_bp_size && resident->key == resident_key &&
                   resident_key.operation_identity == operation_identity;
#endif
        }

        /** @return Whether the completed GPU target has released its mapped source reader. */
        [[nodiscard]] bool sealed_resident_ready() const noexcept {
            return completed_resident_ready() && !persisted_fragment_reader &&
                   !persisted_fragment_mapping_guard;
        }

        /**
         * Release the stored source after its GPU index is complete. Destroy the
         * reader before the mapping. Failed checks leave both objects intact.
         */
        [[nodiscard]] bool seal_persisted_resident_and_release_source() noexcept {
            if (!persisted || !source_ready() || !completed_resident_ready())
                return false;
            persisted_fragment_reader.reset();
            persisted_fragment_mapping_guard.reset();
            return sealed_resident_ready();
        }

        /** @return Whether the GPU target  is complete and its source reader is released. */
        [[nodiscard]] bool ready() const noexcept {
            return sealed_resident_ready();
        }
    };

    /**
     * @brief Reuse CPU target preparation across query batches for one target row.
     * Keep an index group for each minimizer configuration used by those queries.
     */
    struct cpu_prepared_target {
        std::int64_t target_rowno = -1;
        std::uint64_t target_bp_size = 0;
        std::uint64_t query_count = 0;
        /* Keep the statement plan and selected Highway kernels with the target.
         * Later query batches check this binding before reusing its preparation. */
        pipe::statement_execution_plan_owner execution_plan{};
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
        /* These permits cover all groups. Default move assignment replaces the
         * old groups first, and the  destructor explicitly frees group storage
         * before releasing their permits. */
        std::uint64_t query_union_admission_bytes = 0;
        std::optional<bioseq::parallelism::memory_permit> query_union_permit{};
        std::uint64_t query_sketch_cache_admission_bytes = 0;
        std::optional<bioseq::parallelism::memory_permit> query_sketch_cache_permit{};

        cpu_prepared_target() = default;
        cpu_prepared_target(const cpu_prepared_target&) = delete;
        cpu_prepared_target& operator=(const cpu_prepared_target&) = delete;
        cpu_prepared_target(cpu_prepared_target&&) noexcept = default;
        cpu_prepared_target& operator=(cpu_prepared_target&&) noexcept = default;

        /** @return Whether both Highway plans and selected CPU targets match. */
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

        /** @return Whether this target shares the requested statement plan and CPU kernel, binding. */
        [[nodiscard]] bool
        highway_binding_matches(const pipe::statement_execution_plan_owner& requested_plan) const noexcept {
            if (!execution_plan || !requested_plan ||
                execution_plan.get() != requested_plan.get() ||
                execution_plan.owner_before(requested_plan) ||
                requested_plan.owner_before(execution_plan)) {
                return false;
            }
            const auto& params = execution_plan->frozen_parameters;
            return highway_kernels != nullptr && highway_kernels == params.highway_kernels &&
                   same_highway_dispatch_identity(highway_dispatch,
                                                  highway_executed_target,
                                                  params.highway_dispatch,
                                                  params.highway_executed_target);
        }

        /**
         * @brief Save the seleted Highway plan on first use
         * @param[in] requested_plan Statement plan with valid CPU kernels.
         * @return True if the binding was installed or already matches this plan.
         */
        [[nodiscard]] bool bind_highway_dispatch_once(pipe::statement_execution_plan_owner requested_plan) noexcept {
            if (!requested_plan) {
                return false;
            }
            const auto& params = requested_plan->frozen_parameters;
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
                return highway_binding_matches(requested_plan);
            execution_plan = std::move(requested_plan);
            highway_kernels = params.highway_kernels;
            highway_dispatch = params.highway_dispatch;
            highway_executed_target = params.highway_executed_target;
            return true;
        }

        /** @brief Free query groups and their storage before releasing the memory permits. */
        void release_query_union_state_noexcept() noexcept {
            std::vector<cpu_prepared_index_group>{}.swap(groups);
            query_union_admission_bytes = 0;
            query_union_permit.reset();
            query_sketch_cache_admission_bytes = 0;
            query_sketch_cache_permit.reset();
        }

        /**
         * @brief Free a group's legacy sketch union and  reduce its memory reservation.
         * @return False if the stored representation or accounting is inconsistent.
         */
        [[nodiscard]] bool
        release_group_query_hash_union_noexcept(cpu_prepared_index_group& group) noexcept {
            const std::uint64_t released_bytes = bioseq::common::saturating_mul(
                group.query_hash_union.capacity(),
                static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            if (released_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                !query_union_permit.has_value() || query_union_admission_bytes < released_bytes ||
                !group.canonical_query_hash_union_u32.empty() ||
                group.canonical_query_hash_union_u32.capacity() != 0u ||
                !group.canonical_query_hash_union_u64.empty() ||
                group.canonical_query_hash_union_u64.capacity() != 0u ||
                group.canonical_query_hash_width != 0u) {
                return false;
            }
            // The reader has copied the hashes it needs. Free this sketch array
            // before reducing the permit that covers it.
            std::vector<minimap_sketch_data>{}.swap(group.query_hash_union);
            query_union_admission_bytes -= released_bytes;
            query_union_permit->shrink_to(query_union_admission_bytes);
            return true;
        }

        /** Free query-hash arrays after a reader has copied them, then shrink their permit. */
        [[nodiscard]] bool release_group_canonical_query_hashes_noexcept(
            cpu_prepared_index_group& group) noexcept {
            const std::uint64_t u32_bytes = bioseq::common::saturating_mul(
                group.canonical_query_hash_union_u32.capacity(), sizeof(std::uint32_t));
            const std::uint64_t u64_bytes = bioseq::common::saturating_mul(
                group.canonical_query_hash_union_u64.capacity(), sizeof(std::uint64_t));
            const std::uint64_t released_bytes =
                bioseq::common::saturating_add(u32_bytes, u64_bytes);
            const bool one_width = group.canonical_query_hash_width_bytes() != 0u;
            if (!one_width || !group.query_hash_union.empty() ||
                group.query_hash_union.capacity() != 0u ||
                u32_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                u64_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                released_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                !query_union_permit.has_value() ||
                query_union_admission_bytes < released_bytes) {
                return false;
            }
            std::vector<std::uint32_t>{}.swap(group.canonical_query_hash_union_u32);
            std::vector<std::uint64_t>{}.swap(group.canonical_query_hash_union_u64);
            group.canonical_query_hash_width = 0u;
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

        /** @return Whether every prepared group has finished query-set validation. */
        [[nodiscard]] bool query_set_validation_ready() const noexcept {
            return std::ranges::none_of(groups, [](const cpu_prepared_index_group& group) noexcept {
                return group.compact_direct_query_set_validation_pending;
            });
        }
    };

    /**
     * @brief Keep a grouped-SoA GPU target alive across query batches.
     * The resident owns descriptor, control, and  position buffers. resident_view
     * borrows tyose buffers for dispatch.
     */
    struct gpu_prepared_grouped_soa_target {
        pipe::statement_execution_plan_owner execution_plan{};
        std::int64_t target_rowno = -1;
        std::uint64_t target_bp_size = 0;
        std::uint64_t query_count = 0;
        std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target> resident{};
        bioseq::gpu::vulkan::gpu_grouped_soa_resident_view resident_view{};
        bioseq::storage::indexing::minimizer_generation_read_stats persisted_read_stats{};
        std::uint64_t operation_identity = 0;
        std::uint64_t dispatched_query_count = 0;
        std::uint64_t dispatch_waves = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        std::optional<bioseq::indexing::minimizer_fragment_generation_execution_receipt>
            transient_generation_receipt{};
        std::optional<bioseq::indexing::minimizer_generation_publication_binding>
            transient_generation_publication{};
#endif
        bool persisted = false;
        bool canonical_empty = false;
        bool persisted_read_stats_published = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        bool transient_generation_stats_published = false;
#endif
        bool resident_materialization_stats_published = false;

        gpu_prepared_grouped_soa_target() = default;
        gpu_prepared_grouped_soa_target(const gpu_prepared_grouped_soa_target&) = delete;
        gpu_prepared_grouped_soa_target& operator=(const gpu_prepared_grouped_soa_target&) = delete;
        gpu_prepared_grouped_soa_target(gpu_prepared_grouped_soa_target&&) noexcept = default;
        gpu_prepared_grouped_soa_target&
        operator=(gpu_prepared_grouped_soa_target&&) noexcept = default;

        /** @return Whether the target has a plan and source identity for GPU preparation. */
        [[nodiscard]] bool source_ready() const noexcept {
            return execution_plan && target_rowno >= 0 && target_bp_size != 0u &&
                   query_count != 0u && operation_identity != 0u &&
                   (persisted ? resident != nullptr : true);
        }

        /** @brief Check that the GPU buffer views match the completed target and its identity. */
        [[nodiscard]] bool ready() const noexcept {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const bool generation_proof = persisted
                ? !transient_generation_receipt.has_value() &&
                      !transient_generation_publication.has_value()
                : transient_generation_receipt.has_value() &&
                      transient_generation_publication.has_value() &&
                      transient_generation_receipt->publication_accepted(
                          *transient_generation_publication);
            if (!generation_proof)
                return false;
#endif
            return source_ready() && resident != nullptr &&
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
                   resident_view.cold_load_identity == resident->identity().cold_load_identity &&
                   canonical_empty == (resident_view.position_count == 0u);
        }
    };

    /**
     * @brief Test moving, clearing, and preparing the same CPU target twice.
     * Check that borrowed hash views survive moves and that releasing storage
     * also releases its memory reservation. PostgreSQL calls are unnecessary.
     * @return True when all ownership and query-validation checks pass.
     */
    [[nodiscard]] static bool cpu_prepared_target_same_object_reprepare_probe() {
        /* Moving a query, sketch must clear the source's metadata and permit */
        {
            bioseq::parallelism::memory_admission_controller admission;
            auto permit = admission.try_reserve(0u);
            if (!permit) {
                return false;
            }
            cpu_prepared_query_sketch source;
            source.query_ordinal = 17u;
            source.query_bp_size = 101u;
            source.parallel_preparation_permit = std::move(*permit);
            cpu_prepared_query_sketch moved{std::move(source)};
            if (source.query_ordinal != 0u || source.query_bp_size != 0u ||
                source.parallel_preparation_permit.has_value() || source.minimizers.valid() ||
                source.minimizers.capacity_bytes() != 0u || moved.query_ordinal != 17u ||
                moved.query_bp_size != 101u || !moved.parallel_preparation_permit.has_value()) {
                return false;
            }
            cpu_prepared_query_sketch assigned;
            assigned = std::move(moved);
            if (moved.query_ordinal != 0u || moved.query_bp_size != 0u ||
                moved.parallel_preparation_permit.has_value() || moved.minimizers.valid() ||
                moved.minimizers.capacity_bytes() != 0u || assigned.query_ordinal != 17u ||
                assigned.query_bp_size != 101u ||
                !assigned.parallel_preparation_permit.has_value()) {
                return false;
            }
        }
        /* Moving the  target must preserve the array borrowed by its hash view. */
        {
            cpu_prepared_target source;
            source.groups.emplace_back();
            auto& canonical = source.groups.front().canonical_query_hash_union_u32;
            canonical = {3u, 7u, 11u};
            source.groups.front().canonical_query_hash_width = sizeof(std::uint32_t);
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
        /* Reject inconsistent query counts and a second publication. Also test
         * deriving the same checksums through the synchronous fallback. */
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
                group.canonical_query_hash_union_u64.capacity() != 0u ||
                publish_compact_direct_query_set_stats(validation_target, group, rejected) ||
                !group.compact_direct_query_set_validation_pending ||
                !publish_compact_direct_query_set_stats(
                    validation_target, group, query_set_stats) ||
                publish_compact_direct_query_set_stats(validation_target, group, query_set_stats) ||
                !validation_target.query_set_validation_ready() ||
                group.canonical_query_hash_union_u32.capacity() != 0u ||
                group.canonical_query_hash_union_u64.capacity() != 0u ||
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
                fallback_group.canonical_query_hash_union_u64.capacity() != 0u ||
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

    /** @return Whether exactly one stored-index reader was selected. */
    [[nodiscard]] static constexpr bool exactly_one_native_persisted_reader(
        const bool fragment, const bool grouped_soa, const bool hash_major) noexcept {
        return static_cast<unsigned>(fragment) + static_cast<unsigned>(grouped_soa) +
                   static_cast<unsigned>(hash_major) ==
               1u;
    }

    /** @return Whether to try grouped-SoA awter an absent or inapplicable hash-major index, before payload reads. */
    [[nodiscard]] static constexpr bool
    hash_major_grouped_soa_fallback_allowed(const bool head_absent,
                                            const bool profile_applicable,
                                            const bool stream_io_started) noexcept {
        return !stream_io_started && (head_absent || !profile_applicable);
    }

    /**
     * @brief Adjust unchanged minimizer defaults for a short sequence pair.
     * @param[in] base Statement parameters.
     * @param[in] min_bp Length  of the shorter sequence in bases.
     * @return The parameters to use for this pair.
     */
    [[nodiscard]] static minimap2_parameters
    effective_params_for_pair(const minimap2_parameters& base, std::size_t min_bp) noexcept;

    /**
     * @brief Combine query minimizers for reuse by a shared CPU target.
     * Flat sequences and expanded slices use the same typed preparation
     * functions. Saved arrays must fit their memory permits.
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
        pipe::statement_execution_plan_owner execution_plan,
        bool include_persisted_parameter_group,
        cpu_query_union_storage union_storage,
        const cpu_preparation_typmod_operations& typmod_ops,
        cpu_prepared_target& prepared_target);

    /**
     * @brief Open a compatible stored index and save its CPU reader.
     * Check it against the source visible to the executor snapshot and the
     * statement's minimizer settings. Prepare query hashes for that reader.
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
        pipe::statement_execution_plan_owner execution_plan,
        const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
        const cpu_preparation_typmod_operations& typmod_ops,
        cpu_prepared_target& prepared_target);

    /**
     * @brief Record deferred compact-direct  query counts and checksums.
     * @return True if the pending group accepted the complete, query-set result.
     */
    [[nodiscard]] static bool publish_compact_direct_query_set_stats(
        cpu_prepared_target& prepared_target,
        cpu_prepared_index_group& group,
        const bioseq::indexing::compact_direct_query_set_stats& query_set_stats) noexcept;

    /** @brief Finish pending query-set validation from the saved query sketches. */
    static void finalize_pending_compact_direct_query_set_validation_fallback(
        cpu_prepared_target& prepared_target);

    /** @brief Read and upload a compatible stored MPS index for Vulkan seeding. */
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
        pipe::statement_execution_plan_owner execution_plan,
        const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
        gpu_prepared_grouped_soa_target& prepared_target);

    /** @brief Map a compatible stored MPF index for later uplad to Vulkan. */
    void prepare_gpu_persisted_mpf_target(
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
        pipe::statement_execution_plan_owner execution_plan,
        const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
        gpu_prepared_mpf_target& prepared_target);

    /**
     * @brief Carry PostgreSQL readers and shared target state into alignment
     * Locators and expanded slices are read through this context. Flat queries
     * can use their resident bytes directly.
     */
    struct scanner_pg_context {
        /// Keep the same statement plan across  all groups of dispatched rows.
        pipe::statement_execution_plan_owner execution_plan{};
        using gpu_mpf_target_owner = gpu_prepared_mpf_target;
        using gpu_mps_target_owner = gpu_prepared_grouped_soa_target;
        /**
         * @brief Prepare a shared CPU target after Highway kernel selection.
         * The dispatch buffer installs this synchronous callback so query
         * preparation uses the same kernels as later alignment.
         */
        struct highway_preparation_hook final {
            /** @brief Callback for preparing the statement's shared CPU target. */
            using callback_function = void (*)(void*,
                                               scanner_pg_context&,
                                               pipe::statement_execution_plan_owner,
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

        /**
         * @brief Prepare a separate Vulkan target for each ordinary scan row.
         * Each callback fills a caller-owned object and keeps the storage it
         * needs. The caller keeps that object through alignment of the row.
         * Batch joins instead pass one prepared target shared by many queries.
         */
        struct ordinary_vulkan_target_preparation_hook final {
            using mpf_callback_function = void (*)(
                void*,
                scanner_pg_context&,
                std::size_t,
                std::uint64_t,
                std::uint8_t,
                std::uint8_t,
                pipe::statement_execution_plan_owner,
                gpu_mpf_target_owner&);
            using mps_callback_function = void (*)(
                void*,
                scanner_pg_context&,
                std::size_t,
                std::uint64_t,
                std::uint8_t,
                std::uint8_t,
                pipe::statement_execution_plan_owner,
                gpu_mps_target_owner&);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            using map_persisted_read_stats_function = void (*)(
                ::bioseq::pipeline_stats::pipeline_operational_stats&,
                const bioseq::storage::indexing::minimizer_generation_read_stats&) noexcept;
#endif

            void* state = nullptr;
            mpf_callback_function prepare_mpf = nullptr;
            mps_callback_function prepare_mps = nullptr;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            map_persisted_read_stats_function map_persisted_read_stats = nullptr;
#endif

            /** @return True when no ordinary-scan preparation callback is installed. */
            [[nodiscard]] constexpr bool empty() const noexcept {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                return state == nullptr && prepare_mpf == nullptr && prepare_mps == nullptr &&
                       map_persisted_read_stats == nullptr;
#else
                return state == nullptr && prepare_mpf == nullptr && prepare_mps == nullptr;
#endif
            }

            /** @return Whether the state and all callbacks required by  this build are present. */
            [[nodiscard]] constexpr bool valid() const noexcept {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                return state != nullptr && prepare_mpf != nullptr && prepare_mps != nullptr &&
                       map_persisted_read_stats != nullptr;
#else
                return state != nullptr && prepare_mpf != nullptr && prepare_mps != nullptr;
#endif
            }
        };

        highway_preparation_hook highway_preparation{};
        ordinary_vulkan_target_preparation_hook ordinary_vulkan_target_preparation{};
        /* Track asynchronous jobs under the statement's slice owner. */
        bioseq::parallelism::tracked_job_observer async_job_observer{};
        /* Sequence views borrow these allocation observers. Keep them alive, until
         * process_sequences returns and all tracked tasks have finished. */
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
        /* Borrow the current group's read session until alignment and GPU work finish. */
        pg_sequence_read_session* read_session = nullptr;
        // The executor owns this registered MVCC snaphot. Dispatch borrows it
        // without changing its registration.
        Snapshot snapshot = nullptr;
        // Borrowed storage-read context for ordinary-scan Vulkan preparation.
        // Its owner keeps it alive until the  preparation callbacks return.
        const bioseq::storage::indexing::borrowed_executor_read_context*
            executor_read_context = nullptr;
        // The scan owns this provider Use it to read locator-backed targets.
        bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
        // Borrowed statement counters for GPU operations on stored indexes.
        ::bioseq::pipeline_stats::persisted_gpu_operation_accumulator*
            persisted_gpu_operation_stats_sink = nullptr;
        // Shared statement timer used by queries and workers.
        downstream_active_wall_tracker* downstream_active_wall = nullptr;
        // Optional target locators. Rows without a locator use a flat header
        // or an expanded slice supplied below.
        std::array<std::optional<bioseq::storage::sequence_locator>, dispatch_batch_capacity>
            target_locators{};
        // Query-column locators use the scan provider with their own attribute
        // number and storage width.
        std::array<std::optional<bioseq::storage::sequence_locator>, dispatch_batch_capacity>
            query_locators{};
        /* Expanded slice wrappers saved by the batch buffer. Each nonzero
         * Datum has a checked descriptor. CPU dispatch reads these through
         * lazy sequence views while  the batch keeps the wrappers alive. */
        std::array<Datum, dispatch_batch_capacity> target_recipe_datums{};
        std::array<Datum, dispatch_batch_capacity> query_recipe_datums{};
        std::array<pg_sequence_descriptor, dispatch_batch_capacity> target_recipe_descriptors{};
        std::array<pg_sequence_descriptor, dispatch_batch_capacity> query_recipe_descriptors{};
        // Physical sequence row numbers used to locate stored target indexes.
        std::array<std::int64_t, dispatch_batch_capacity> target_rownos{};
        std::array<std::uint64_t, dispatch_batch_capacity> query_ordinals{};
        bool explicit_query_ordinals = false;
        bool shared_target_batch = false;
        bool record_target_operational_outcomes = true;
        std::shared_ptr<cpu_prepared_target> cpu_prepared_target{};
        gpu_prepared_mpf_target* gpu_prepared_mpf_target = nullptr;
        gpu_prepared_grouped_soa_target* gpu_prepared_grouped_soa_target = nullptr;
        // Record the index source separately from wheher alignment found a match.
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
        // Selected index layout for this target column. Record whether it came
        // from a catalog row or the default so statistics can distinguish them.
        std::string active_index;
        bioseq::storage::indexing::active_read_policy_source active_index_policy_source =
            bioseq::storage::indexing::active_read_policy_source::unresolved;
    };
    /**
     * @brief Align the first batch_size rows with, the selected statement plan.
     * Use one shared query or a per-row query array. The optional scanner
     * context supplies deferred inputs and index readers. Stored-index use
     * follows its  explicit read requirement.
     * @return Matches grouped by the identifiers in query_ids.
     */
    [[nodiscard]] match_result_map process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const bioseq::pg_types::seq* query_hdr,
        const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
        const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
        const std::array<int32_t, dispatch_batch_capacity>& query_ids,
        pipe::statement_execution_plan_owner execution_plan,
        batch_invocation_role invocation_role,
        std::uint64_t broadcast_query_ordinal,
        ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
        scanner_pg_context* scanner_pg_ctx = nullptr);
};

/** Query-preparation functions specialized for the generated dispatch types. */
export namespace cpu_preparation_detail {

template <class p_alphabet, std::size_t p_width>
[[nodiscard]] const batch_processor::cpu_preparation_typmod_operations& typmod_ops() noexcept;

} // namespace cpu_preparation_detail

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
