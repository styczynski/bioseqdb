/**
 * @file typed_support_parts/resident_targets.cppm
 * @brief Defines transient indexed alignment and admitted resident-target materialization.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_targets;

import std;
export import bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.heap_allocation;
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
import bioseq.postgres.storage.indexing.external_file_producer_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.pipeline.minimizer_execution_contract;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.storage.scanner;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Flush one transient-MPF phase failure without replacing its owning failure. */
inline void emit_transient_mpf_failure(
    const char* site,
    std::uint64_t target_bp,
    std::uint64_t event_count,
    std::uint64_t index_peak_bytes = 0u,
    std::uint64_t permit_bytes = 0u,
    std::uint64_t position_bytes = 0u,
    std::uint64_t retained_position_bytes = 0u,
    std::uint64_t table_owner_requirement_bytes = 0u,
    std::uint64_t table_owner_bytes = 0u,
    std::uint64_t planned_table_bytes = 0u,
    std::uint64_t retained_table_bytes = 0u,
    std::uint64_t peak_table_bytes = 0u,
    std::uint64_t shared_owner_bytes = 0u,
    const bioseq::parallelism::memory_admission_failure* failure = nullptr) noexcept {
    const auto reason = failure != nullptr && failure->has_failure()
                            ? bioseq::parallelism::memory_admission_failure_reason_name(
                                  failure->reason)
                            : std::string_view{"none"};
    try {
        bioseq_log_progress<logging_level::warning, logging_target::file>(
            "[bioseq.transient_mpf.failure] site={} target_bp={} event_count={} "
            "index_peak_bytes={} permit_bytes={} position_bytes={} "
            "retained_position_bytes={} table_owner_requirement_bytes={} "
            "table_owner_bytes={} planned_table_bytes={} retained_table_bytes={} "
            "peak_table_bytes={} shared_owner_bytes={} admission_reason={} "
            "requested_growth_bytes={} reserved_bytes={} budget_ceiling_bytes={} "
            "allocatable_after_reservations_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} process_soft_cap_bytes={}",
            site,
            static_cast<unsigned long long>(target_bp),
            static_cast<unsigned long long>(event_count),
            static_cast<unsigned long long>(index_peak_bytes),
            static_cast<unsigned long long>(permit_bytes),
            static_cast<unsigned long long>(position_bytes),
            static_cast<unsigned long long>(retained_position_bytes),
            static_cast<unsigned long long>(table_owner_requirement_bytes),
            static_cast<unsigned long long>(table_owner_bytes),
            static_cast<unsigned long long>(planned_table_bytes),
            static_cast<unsigned long long>(retained_table_bytes),
            static_cast<unsigned long long>(peak_table_bytes),
            static_cast<unsigned long long>(shared_owner_bytes),
            reason,
            static_cast<unsigned long long>(failure != nullptr ? failure->requested_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr ? failure->reserved_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->budget_ceiling_bytes
                                                : 0u),
            static_cast<unsigned long long>(
                failure != nullptr ? failure->allocatable_after_reservations_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_footprint_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_available_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_total_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_soft_cap_bytes
                                                : 0u));
    } catch (...) {
        // Diagnostics must not replace the existing PostgreSQL or allocation failure.
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
#endif

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** One generated physical query owner plus optional native-width grouped-reader keys. */
struct transient_physical_query {
    // Permit first: both hash vectors and the generated shared owner die before accounting.
    bioseq::parallelism::memory_permit permit;
    bioseq::minimizers::minimizer_physical_batch_owner minimizers;
    pipe::minimizer_range_execution_stats execution_stats{};
    bioseq::minimizers::minimizer_physical_layout_selection layout{};
    std::vector<std::uint32_t> hashes_u32;
    std::vector<std::uint64_t> hashes_u64;

    transient_physical_query() = default;
    transient_physical_query(const transient_physical_query&) = delete;
    transient_physical_query& operator=(const transient_physical_query&) = delete;
    transient_physical_query(transient_physical_query&& other) noexcept
        : permit(std::move(other.permit)), minimizers(std::move(other.minimizers)),
          execution_stats(std::exchange(other.execution_stats, {})),
          layout(std::exchange(other.layout, {})) {
        hashes_u32.swap(other.hashes_u32);
        hashes_u64.swap(other.hashes_u64);
    }
    transient_physical_query& operator=(transient_physical_query&&) = delete;
};

/** Raise one checked PostgreSQL error for a transient generation contract failure. */
[[noreturn]] inline void raise_transient_generation_error(
    std::string_view stage,
    bioseq::indexing::minimizer_generation_encoding_error error) {
    const int sqlstate =
        error == bioseq::indexing::minimizer_generation_encoding_error::admission
            ? ERRCODE_OUT_OF_MEMORY
            : ERRCODE_DATA_CORRUPTED;
    const std::string diagnostic = std::string{stage} +
                                   " rejected grouped minimizer generation (wire_error=" +
                                   std::to_string(static_cast<unsigned>(error)) + ')';
    raise_typed_dispatch_pg_error_as_cpp(sqlstate, diagnostic.c_str());
}

/** Collect one query through the generated record selected before instantiation. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] transient_physical_query prepare_transient_physical_query(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    const minimap2_parameters& params,
    std::uint8_t native_storage_unit_byte_width,
    bool retain_sorted_hashes) {
    if ((native_storage_unit_byte_width != sizeof(std::uint32_t) &&
         native_storage_unit_byte_width != sizeof(std::uint64_t)) ||
        params.kmer_size <= 0 || params.window_size <= 0 ||
        static_cast<std::uint64_t>(params.kmer_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        static_cast<std::uint64_t>(params.window_size) >
            (std::numeric_limits<std::uint16_t>::max)()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient query minimizer geometry is outside the generated registry");
    }
    const auto layout = pipe::select_minimizer_execution_layout_prefer_native<p_alphabet>(
        params.minimizer_algorithm,
        static_cast<std::uint64_t>(query_view.bp_size()),
        static_cast<std::uint16_t>(params.kmer_size),
        static_cast<std::uint16_t>(params.window_size),
        static_cast<std::uint8_t>(native_storage_unit_byte_width * 8u));
    const auto backend =
        pipe::minimizer_sketch_backend_for_effective_algorithm(layout.effective_algorithm);
    if (!backend || *backend == minimizer_sketch_backend::vulkan_segment) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient host query has no generated minimizer implementation");
    }
    return bioseq::minimizers::visit_minimizer_physical_layout(
        layout,
        [query_view,
         &params,
         layout,
         backend = *backend,
         retain_sorted_hashes]<class p_record>() -> transient_physical_query {
            if (!pipe::minimizer_execution_layout_matches_record<p_record, p_alphabet>(
                    layout,
                    static_cast<std::uint16_t>(params.kmer_size),
                    static_cast<std::uint16_t>(params.window_size)) ||
                !is_minimizer_query_length_representable<p_record>(query_view.bp_size())) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_FEATURE_NOT_SUPPORTED,
                    "transient query cannot use its generated minimizer record");
            }
            auto admitted =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
            if (!admitted) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "transient physical query owner cannot be admitted");
            }
            admitted_sketch_collector<p_record, bioseq::parallelism::memory_permit> collector{
                std::move(*admitted),
                bioseq::parallelism::k_vector_preallocation_headroom_factor};
            pipe::minimizer_range_execution_stats stats =
                pipe::minimizer_dispatch_indexed_query_whole<p_record>(
                    collector,
                    query_view,
                    static_cast<std::size_t>(params.window_size),
                    static_cast<std::size_t>(params.kmer_size),
                    backend,
                    params.highway_kernels,
                    std::addressof(layout));
            if (!collector.is_ok() ||
                !pipe::minimizer_range_stats_matches_effective_algorithm(
                    layout.effective_algorithm,
                    stats,
                    /*require_nonempty_vector_work=*/false,
                    pipe::exact_simd_range_route::require_continuation)) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    "transient query execution stats disagree with its generated backend");
            }
            auto released = std::move(collector).release_owned_storage();
            bioseq::parallelism::memory_permit storage_permit = std::move(released.second);
            std::vector<p_record> records = std::move(released.first);
            const std::uint64_t record_capacity_bytes = bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(records.capacity()),
                static_cast<std::uint64_t>(sizeof(p_record)));
            if (record_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                record_capacity_bytes != storage_permit.bytes()) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    "transient query vector capacity disagrees with admission");
            }
            transient_physical_query result;
            result.minimizers =
                bioseq::minimizers::minimizer_physical_batch_owner::from_owned_host(
                    layout, std::move(records), storage_permit);
            result.permit = std::move(storage_permit);
            result.execution_stats = stats;
            result.layout = layout;
            if (retain_sorted_hashes) {
                const std::uint64_t physical_owner_bytes = result.minimizers.capacity_bytes();
                if (layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u32) {
                    if (!ensure_transient_vector_capacity(result.permit,
                                                          result.hashes_u32,
                                                          result.minimizers.size(),
                                                          physical_owner_bytes)) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_OUT_OF_MEMORY,
                            "transient H32 query-hash owner cannot be admitted");
                    }
                    bool hashes_fit = true;
                    result.minimizers.view().for_each_decoded(
                        [&hashes = result.hashes_u32,
                         &hashes_fit](std::size_t,
                                     bioseq::minimizers::minimizer_physical_record_value value) {
                            if (value.kmer() >
                                (std::numeric_limits<std::uint32_t>::max)()) {
                                hashes_fit = false;
                                return;
                            }
                            hashes.push_back(static_cast<std::uint32_t>(value.kmer()));
                        });
                    if (!hashes_fit) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "transient H32 query owner contains a wide hash");
                    }
                    std::ranges::sort(result.hashes_u32);
                    result.hashes_u32.erase(std::ranges::unique(result.hashes_u32).begin(),
                                            result.hashes_u32.end());
                } else if (
                    layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u64 ||
                    (bioseq::minimizers::minimizer_physical_record_detail::
                         k_has_32_bit_record &&
                     layout.hash_width ==
                         bioseq::minimizers::minimizer_scalar_width::u16)) {
                    if (!ensure_transient_vector_capacity(result.permit,
                                                          result.hashes_u64,
                                                          result.minimizers.size(),
                                                          physical_owner_bytes)) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_OUT_OF_MEMORY,
                            "transient H64 query-hash owner cannot be admitted");
                    }
                    result.minimizers.view().for_each_decoded(
                        [&hashes = result.hashes_u64](
                            std::size_t,
                            bioseq::minimizers::minimizer_physical_record_value value) {
                            hashes.push_back(value.kmer());
                        });
                    std::ranges::sort(result.hashes_u64);
                    result.hashes_u64.erase(std::ranges::unique(result.hashes_u64).begin(),
                                            result.hashes_u64.end());
                } else {
                    raise_typed_dispatch_pg_error_as_cpp(
                        ERRCODE_FEATURE_NOT_SUPPORTED,
                        "transient query selected an unsupported hash width");
                }
            }
            const std::uint64_t hash_capacity_bytes = bioseq::common::saturating_add(
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(result.hashes_u32.capacity()),
                    static_cast<std::uint64_t>(sizeof(std::uint32_t))),
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(result.hashes_u64.capacity()),
                    static_cast<std::uint64_t>(sizeof(std::uint64_t))));
            const std::uint64_t retained = bioseq::common::saturating_add(
                result.minimizers.capacity_bytes(), hash_capacity_bytes);
            if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
                retained != result.permit.bytes()) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    "transient physical query ownership equation failed");
            }
            return result;
        });
}

/** Attach only operation-originated query stats after the indexed suffix succeeds. */
inline void record_transient_query_execution(
    match_result_collection& collection,
    const transient_physical_query& query) {
    if (!pipe::minimizer_range_stats_matches_effective_algorithm(
            query.layout.effective_algorithm,
            query.execution_stats,
            /*require_nonempty_vector_work=*/false,
            pipe::exact_simd_range_route::require_continuation)) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "transient query receipt changed before result publication");
    }
    if (query.execution_stats.scalar_available) {
        collection.record_scalar_query_minimizer_execution_stats(
            query.execution_stats.scalar);
    }
    if (query.execution_stats.exact_simd_available) {
        collection.record_exact_simd_query_execution_stats(
            query.execution_stats.exact_simd);
    }
    if (query.execution_stats.bounded_nthash_available) {
        collection.record_bounded_nthash_query_execution_stats(
            query.execution_stats.bounded_nthash);
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    collection.record_completed_minimizer_algorithm_identity(
        query.layout.effective_algorithm);
#endif
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** Retain one authenticated ordinary transient-target authority for job finalization. */
inline void record_transient_cpu_target_generation_authority(
    match_result_collection& collection,
    const transient_cpu_target_generation_representation representation,
    bioseq::indexing::minimizer_sequence_generation_execution_receipt receipt) {
    if (!collection.record_transient_cpu_target_generation_receipt(
            representation, std::move(receipt))) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient CPU target generation receipt is invalid or duplicated");
    }
}

/**
 * @brief Retains operation-originated bounded target counters after a transient suffix succeeds.
 * @param collection Completed suffix receiving the one physical target operation.
 * @param stats Authenticated index producer counters, projected without numerical changes.
 */
inline void record_transient_bounded_target_execution(
    match_result_collection& collection,
    const bioseq::indexing::packed_bounded_nthash_index_execution_stats& stats) {
    if (!stats.is_internally_consistent(/*require_nonempty_vector_work=*/false) ||
        collection.bounded_nthash_target_execution_stats_available ||
        collection.bounded_nthash_target_execution_stats !=
            bioseq::minimizers::bounded_nthash_execution_stats{}) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient bounded target counters are inconsistent or already recorded");
    }
    collection.record_bounded_nthash_target_execution_stats({
        .scheme_id = stats.scheme_id,
        .kmer_size = stats.kmer_size,
        .window_size = stats.window_size,
        .nthash_rotation = stats.nthash_rotation,
        .source_storage_unit_bytes = stats.source_storage_unit_bytes,
        .input_symbols = stats.input_symbols,
        .input_storage_units = stats.input_storage_units,
        .segments_combined = stats.segments_combined,
        .logical_streams = stats.logical_streams,
        .physical_vector_lanes = stats.physical_vector_lanes,
        .vector_registers_per_step = stats.vector_registers_per_step,
        .packed_decode_batches = stats.packed_decode_batches,
        .packed_decode_lane_visits = stats.packed_decode_lane_visits,
        .hash_initialization_batches = stats.hash_initialization_batches,
        .rolling_hash_batches = stats.rolling_hash_batches,
        .hash_table_lookup_batches = stats.hash_table_lookup_batches,
        .canonical_sum_batches = stats.canonical_sum_batches,
        .window_update_batches = stats.window_update_batches,
        .suffix_rebuild_batches = stats.suffix_rebuild_batches,
        .emission_decision_batches = stats.emission_decision_batches,
        .emission_mask_store_batches = stats.emission_mask_store_batches,
        .minimizers_emitted = stats.minimizers_emitted,
        .tied_endpoint_records_emitted = stats.tied_endpoint_records_emitted,
        .stream_boundary_records_buffered = stats.stream_boundary_records_buffered,
        .stream_boundary_records_committed = stats.stream_boundary_records_committed,
        .stream_boundary_duplicates_suppressed = stats.stream_boundary_duplicates_suppressed,
        .scalar_collector_appends = stats.scalar_collector_appends,
        .partial_chunk_callback_batches = stats.partial_chunk_callback_batches,
        .masked_tail_lane_visits = stats.masked_tail_lane_visits,
        .fail_closed_rejections = stats.fail_closed_rejections,
        .scalar_hash_symbols = stats.scalar_hash_symbols,
        .scalar_window_transitions = stats.scalar_window_transitions,
        .scalar_minimum_decisions = stats.scalar_minimum_decisions,
        .fallback_calls = stats.fallback_calls,
        .positions_are_segment_local = stats.positions_are_segment_local,
        .aggregation_mismatch = stats.aggregation_mismatch,
        .saturated = stats.saturated,
    });
}
#endif

/** Generated target layout, persisted identity, and concrete host producer backend. */
struct transient_target_generation_selection {
    bioseq::minimizers::minimizer_physical_layout_selection layout{};
    bioseq::indexing::minimizer_generation_algorithm_identity identity{};
    minimizer_sketch_backend backend = minimizer_sketch_backend::scalar;
};

/** Select the generated target record and its concrete host producer backend. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] transient_target_generation_selection
select_transient_target_generation(const minimap2_parameters& params,
                                   std::uint64_t maximum_position_extent_bp,
                                   std::uint8_t native_storage_unit_byte_width) {
    if ((native_storage_unit_byte_width != sizeof(std::uint32_t) &&
         native_storage_unit_byte_width != sizeof(std::uint64_t)) ||
        params.kmer_size <= 0 || params.window_size <= 0 ||
        static_cast<std::uint64_t>(params.kmer_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        static_cast<std::uint64_t>(params.window_size) >
            (std::numeric_limits<std::uint16_t>::max)()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient target minimizer geometry is outside the generated registry");
    }
    const auto layout = pipe::select_minimizer_execution_layout_prefer_native<p_alphabet>(
        params.minimizer_algorithm,
        maximum_position_extent_bp,
        static_cast<std::uint16_t>(params.kmer_size),
        static_cast<std::uint16_t>(params.window_size),
        static_cast<std::uint8_t>(native_storage_unit_byte_width * 8u));
    const auto identity =
        bioseq::indexing::make_minimizer_generation_algorithm_identity(layout);
    const auto backend =
        pipe::minimizer_sketch_backend_for_effective_algorithm(layout.effective_algorithm);
    if (!identity || !backend || *backend == minimizer_sketch_backend::vulkan_segment) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient host target has no generated minimizer implementation");
    }
    return {
        .layout = layout,
        .identity = *identity,
        .backend = *backend,
    };
}

/** Verify that independently selected query and target records share biological hash identity. */
[[nodiscard]] inline bool transient_query_matches_target_identity(
    const transient_physical_query& query,
    const bioseq::indexing::minimizer_generation_algorithm_identity& target) noexcept {
    return query.layout.requested_algorithm == target.requested_algorithm &&
           query.layout.effective_algorithm == target.effective_algorithm &&
           query.layout.alphabet_id == target.alphabet_id &&
           query.layout.physical_kmer_size == target.physical_kmer_size &&
           query.layout.hash_width == target.hash_width &&
           query.layout.runtime_kmer_size == target.runtime_kmer_size &&
           query.layout.runtime_window_size == target.runtime_window_size;
}

/** Verify a retained generated query layout against a target producer identity. */
[[nodiscard]] inline bool transient_query_layout_matches_target_identity(
    const bioseq::minimizers::minimizer_physical_layout_selection& query,
    const bioseq::indexing::minimizer_generation_algorithm_identity& target) noexcept {
    return query.requested_algorithm == target.requested_algorithm &&
           query.effective_algorithm == target.effective_algorithm &&
           query.alphabet_id == target.alphabet_id &&
           query.physical_kmer_size == target.physical_kmer_size &&
           query.hash_width == target.hash_width &&
           query.runtime_kmer_size == target.runtime_kmer_size &&
           query.runtime_window_size == target.runtime_window_size;
}

/** Spill one target after filtering by the statement's native-width canonical query union. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] auto spill_transient_target_for_prepared_queries(
    opportunistic_sequence_index_storage& storage,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    const minimap2_parameters& params,
    const transient_target_generation_selection& target_generation,
    std::uint8_t alphabet_code,
    const bioseq::indexing::canonical_query_hash_view hashes,
    bioseq::indexing::minimizer_sequence_generation_execution_receipt& receipt) {
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>;
    if (!hashes.storage_matches_width()) [[unlikely]] {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient target preparation has inconsistent canonical query hash storage");
    }
    const auto* kernels = params.highway_kernels == nullptr
                              ? nullptr
                              : std::addressof(params.highway_kernels->minimizers);
    if (hashes.width_bytes == sizeof(std::uint32_t) && hashes.values64.empty()) {
        return sequence_index::spill_generation_replay_runs_serial(
            storage,
            target_view,
            target_generation.backend,
            kernels,
            target_generation.identity,
            /*core_span_bp=*/0u,
            bioseq::indexing::k_minimizer_preferred_segment_span_bp,
            std::addressof(receipt),
            alphabet_code,
            hashes.values32,
            std::span<const std::uint64_t>{},
            sizeof(std::uint32_t));
    }
    if (hashes.width_bytes == sizeof(std::uint64_t) && hashes.values32.empty()) {
        return sequence_index::spill_generation_replay_runs_serial(
            storage,
            target_view,
            target_generation.backend,
            kernels,
            target_generation.identity,
            /*core_span_bp=*/0u,
            bioseq::indexing::k_minimizer_preferred_segment_span_bp,
            std::addressof(receipt),
            alphabet_code,
            std::span<const std::uint32_t>{},
            hashes.values64,
            sizeof(std::uint64_t));
    }
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_DATA_CORRUPTED,
        "transient target preparation has no unambiguous canonical query width");
    pg_unreachable();
}

/** Allocate one shared target facade while charging its allocator-reported control extent. */
template <class p_target, class... p_arguments>
[[nodiscard]] std::shared_ptr<p_target> make_admitted_transient_target_owner(
    bioseq::parallelism::memory_permit& permit,
    std::uint64_t& owner_allocation_bytes,
    p_arguments&&... arguments) {
    if (permit || owner_allocation_bytes != 0u) {
        throw std::logic_error{"transient target owner admission is not fresh"};
    }
    auto acquired =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
    if (!acquired) {
        throw std::bad_alloc{};
    }
    permit = std::move(*acquired);
    bioseq::minimizers::physical_batch_owner_detail::transaction_scope transaction{permit, 0u};
    auto result = std::allocate_shared<p_target>(
        bioseq::minimizers::physical_batch_owner_detail::
            admitted_shared_owner_allocator<p_target>{},
        std::forward<p_arguments>(arguments)...);
    owner_allocation_bytes = transaction.owner_allocation_bytes();
    if (owner_allocation_bytes == 0u || permit.bytes() != owner_allocation_bytes) {
        throw std::logic_error{"transient target shared owner was not physically reconciled"};
    }
    transaction.commit();
    return result;
}

/** One admitted MPF owner; the shared index is destroyed before its accounting permit. */
struct admitted_transient_mpf_index {
    bioseq::parallelism::memory_permit permit;
    std::shared_ptr<bioseq::indexing::minimizer_hashmap_fragment_index> index;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    bioseq::indexing::minimizer_sequence_generation_execution_receipt receipt{};
#endif
};

/** Build one exact query-filtered MPF target under its complete physical admission. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] admitted_transient_mpf_index build_admitted_transient_mpf_index(
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    const minimap2_parameters& params,
    const transient_target_generation_selection& target_generation,
    sequence_size_t total_bp_size,
    std::uint8_t alphabet_code,
    const bioseq::indexing::canonical_query_hash_view hashes) {
    opportunistic_sequence_index_storage storage;
    storage.run_buffer_bytes_value =
        bioseq::settings::index_build::run_buffer_bytes > 0
            ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
            : (std::size_t{256} << 20);
    bioseq::indexing::minimizer_sequence_generation_execution_receipt spill_receipt{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    auto spill = [&]() {
        try {
            return spill_transient_target_for_prepared_queries(
                storage,
                target_view,
                params,
                target_generation,
                alphabet_code,
                hashes,
                spill_receipt);
        } catch (const std::bad_alloc&) {
            emit_transient_mpf_failure("spill_generation_bad_alloc",
                                       static_cast<std::uint64_t>(total_bp_size),
                                       0u);
            throw;
        }
    }();
#else
    auto spill = spill_transient_target_for_prepared_queries(
        storage,
        target_view,
        params,
        target_generation,
        alphabet_code,
        hashes,
        spill_receipt);
#endif
    if (!spill || !spill_receipt.is_spill_valid() ||
        spill_receipt.algorithm_identity != target_generation.identity ||
        spill_receipt.spilled_event_count != spill->execution_receipt().spilled_event_count) {
        raise_transient_generation_error(
            "prepared transient MPF spill",
            spill ? bioseq::indexing::minimizer_generation_encoding_error::replay_mismatch
                  : spill.error());
    }
    const std::uint64_t event_count = spill_receipt.spilled_event_count;
    if (event_count >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        event_count >
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            "prepared transient MPF event count exceeds lookup offset geometry");
    }
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>;
    admitted_transient_mpf_index result;
    bioseq::minimizers::visit_minimizer_physical_layout(
        target_generation.layout,
        [&target_generation,
         &params,
         total_bp_size,
         event_count,
         &spill,
         &storage,
         &result]<class p_record>() {
            if (!pipe::minimizer_execution_layout_matches_record<p_record, p_alphabet>(
                    target_generation.layout,
                    static_cast<std::uint16_t>(params.kmer_size),
                    static_cast<std::uint16_t>(params.window_size))) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "prepared transient MPF selected the wrong physical target record");
            }
            transient_materialized_array<p_record> events;
            if (!prepare_transient_array_exact(events, static_cast<std::size_t>(event_count))) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "prepared transient MPF exact event image cannot be admitted");
            }
            constexpr std::uint64_t replay_segment_span_bp =
                bioseq::indexing::k_minimizer_preferred_segment_span_bp;
            static_assert(replay_segment_span_bp != 0u);
            auto producer = sequence_index::make_generation_replay_producer(
                storage,
                static_cast<std::uint64_t>(total_bp_size),
                std::move(*spill),
                replay_segment_span_bp);
            struct replay_context {
                transient_materialized_array<p_record>* events = nullptr;
                std::uint64_t target_length = 0u;
                std::uint64_t position_segment_span_bp = 0u;
                std::uint64_t previous_segment = 0u;
                std::uint64_t previous_hash = 0u;
                std::uint64_t previous_encoded_position = 0u;
                bool valid = true;
                bool have_previous = false;
            } context{std::addressof(events),
                      static_cast<std::uint64_t>(total_bp_size),
                      replay_segment_span_bp,
                      0u,
                      0u,
                      0u,
                      true,
                      false};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            const bioseq::indexing::canonical_minimizer_event_receiver replay_receiver{
                .context = std::addressof(context),
                .emit_fn = +[](void* erased,
                              const bioseq::indexing::canonical_minimizer_event& event)
                    -> std::expected<void,
                                     bioseq::indexing::minimizer_generation_encoding_error> {
                    auto& state = *static_cast<replay_context*>(erased);
                    const std::uint64_t position = event.encoded_position >> 1u;
                    const std::uint64_t strand = event.encoded_position & 1u;
                    const std::uint64_t segment = position / state.position_segment_span_bp;
                    const bool ordered =
                        !state.have_previous ||
                        std::tuple{state.previous_segment,
                                   state.previous_hash,
                                   state.previous_encoded_position} <=
                            std::tuple{segment, event.hash, event.encoded_position};
                    if (!state.valid || !ordered ||
                        state.events->size == state.events->capacity ||
                        position >= state.target_length) {
                        state.valid = false;
                        return std::unexpected(
                            bioseq::indexing::minimizer_generation_encoding_error::position_range);
                    }
                    const p_record record{event.hash, position, strand};
                    if (record.kmer() != event.hash || record.last_pos() != position ||
                        record.strand() != strand) {
                        state.valid = false;
                        return std::unexpected(
                            bioseq::indexing::minimizer_generation_encoding_error::
                                storage_contract_mismatch);
                    }
                    state.events->values.get()[state.events->size] = record;
                    ++state.events->size;
                    state.previous_segment = segment;
                    state.previous_hash = event.hash;
                    state.previous_encoded_position = event.encoded_position;
                    state.have_previous = true;
                    return {};
                },
            };
            const auto replayed = [&]() {
                try {
                    return producer.scan_sorted_events(
                        replay_segment_span_bp,
                        replay_receiver);
                } catch (const std::bad_alloc&) {
                    emit_transient_mpf_failure("replay_scan_bad_alloc",
                                               static_cast<std::uint64_t>(total_bp_size),
                                               event_count);
                    throw;
                }
            }();
#else
            const auto replayed = producer.scan_sorted_events(
                replay_segment_span_bp,
                bioseq::indexing::canonical_minimizer_event_receiver{
                    .context = std::addressof(context),
                    .emit_fn = +[](void* erased,
                                  const bioseq::indexing::canonical_minimizer_event& event)
                        -> std::expected<
                            void,
                            bioseq::indexing::minimizer_generation_encoding_error> {
                        auto& state = *static_cast<replay_context*>(erased);
                        const std::uint64_t position = event.encoded_position >> 1u;
                        const std::uint64_t strand = event.encoded_position & 1u;
                        const std::uint64_t segment =
                            position / state.position_segment_span_bp;
                        const bool ordered =
                            !state.have_previous ||
                            std::tuple{state.previous_segment,
                                       state.previous_hash,
                                       state.previous_encoded_position} <=
                                std::tuple{segment, event.hash, event.encoded_position};
                        if (!state.valid || !ordered ||
                            state.events->size == state.events->capacity ||
                            position >= state.target_length) {
                            state.valid = false;
                            return std::unexpected(
                                bioseq::indexing::minimizer_generation_encoding_error::
                                    position_range);
                        }
                        const p_record record{event.hash, position, strand};
                        if (record.kmer() != event.hash || record.last_pos() != position ||
                            record.strand() != strand) {
                            state.valid = false;
                            return std::unexpected(
                                bioseq::indexing::minimizer_generation_encoding_error::
                                    storage_contract_mismatch);
                        }
                        state.events->values.get()[state.events->size] = record;
                        ++state.events->size;
                        state.previous_segment = segment;
                        state.previous_hash = event.hash;
                        state.previous_encoded_position = event.encoded_position;
                        state.have_previous = true;
                        return {};
                    },
                });
#endif
            if (!replayed || !context.valid || events.size != event_count ||
                producer.completed_scan_count() != 1u ||
                producer.failed_scan_count() != 0u ||
                producer.illegal_span_transition_count() != 0u ||
                producer.visited_event_count() != event_count) {
                raise_transient_generation_error(
                    "prepared transient MPF replay",
                    replayed
                        ? bioseq::indexing::minimizer_generation_encoding_error::replay_mismatch
                        : replayed.error());
            }
            bioseq::indexing::minimizer_hashmap_fragment_index::sort_source_by_hash_in_place(
                std::span<p_record>{events.values.get(), events.size});

            bioseq::parallelism::memory_permit index_permit;
            std::uint64_t shared_owner_bytes = 0u;
            auto index = make_admitted_transient_target_owner<
                bioseq::indexing::minimizer_hashmap_fragment_index>(
                index_permit,
                shared_owner_bytes,
                bioseq::indexing::minimizer_hashmap_fragment_index::defer_table_allocation);
            const auto table_plan =
                bioseq::indexing::plan_minimizer_hashmap_table_allocation(event_count);
            const std::uint64_t table_owner_requirement =
                bioseq::indexing::minimizer_hashmap_fragment_index::
                    table_owner_allocation_requirement_bytes();
            const std::uint64_t position_bytes = bioseq::common::saturating_mul(
                event_count, static_cast<std::uint64_t>(sizeof(std::uint64_t)));
            bioseq::common::heap_allocation_detail::heap_allocation_plan position_plan{};
            try {
                position_plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(
                    static_cast<std::size_t>(position_bytes));
            } catch (const std::exception&) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_FEATURE_NOT_SUPPORTED,
                    "prepared transient MPF position allocation cannot be proven");
            }
            const std::uint64_t index_peak = bioseq::common::saturating_add(
                shared_owner_bytes,
                bioseq::common::saturating_add(
                    table_owner_requirement,
                    bioseq::common::saturating_add(
                        static_cast<std::uint64_t>(
                            position_plan.allocation_requirement_bytes),
                        table_plan.allocation_bytes)));
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            const bool index_plan_valid =
                table_plan.valid &&
                table_owner_requirement != (std::numeric_limits<std::uint64_t>::max)() &&
                position_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
                index_peak != (std::numeric_limits<std::uint64_t>::max)();
            bioseq::parallelism::memory_admission_failure index_admission_failure{};
            const bool index_admitted =
                index_plan_valid &&
                index_permit.grow_to(index_peak, std::addressof(index_admission_failure));
            if (!index_admitted) {
                emit_transient_mpf_failure(
                    index_plan_valid ? "index_reservation_denied" : "index_plan_invalid",
                    static_cast<std::uint64_t>(total_bp_size),
                    event_count,
                    index_peak,
                    index_permit.bytes(),
                    position_bytes,
                    0u,
                    table_owner_requirement,
                    0u,
                    table_plan.allocation_bytes,
                    0u,
                    0u,
                    shared_owner_bytes,
                    index_plan_valid ? std::addressof(index_admission_failure) : nullptr);
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "prepared transient MPF index replacement cannot be admitted");
            }
#else
            if (!table_plan.valid ||
                table_owner_requirement == (std::numeric_limits<std::uint64_t>::max)() ||
                position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                index_peak == (std::numeric_limits<std::uint64_t>::max)() ||
                !index_permit.grow_to(index_peak)) {
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "prepared transient MPF index replacement cannot be admitted");
            }
#endif
            index->build_presorted_full_table(
                std::span<const p_record>{events.values.get(), events.size}, total_bp_size);
            events.values.reset();
            events.permit.release();
            events.allocation_bytes = 0u;
            events.size = 0u;
            events.capacity = 0u;
            events.permit_released_to_footprint = false;
            const std::uint64_t retained_positions = static_cast<std::uint64_t>(
                bioseq::common::heap_allocation_detail::allocator_reported_bytes(
                    index->positions.data()));
            const std::uint64_t table_owner_bytes = index->table_owner_allocation_bytes();
            const std::uint64_t retained_index = bioseq::common::saturating_add(
                shared_owner_bytes,
                bioseq::common::saturating_add(
                    table_owner_bytes,
                    bioseq::common::saturating_add(retained_positions,
                                                   index->table_allocation_bytes())));
            if (retained_positions < position_bytes ||
                retained_positions >
                    static_cast<std::uint64_t>(position_plan.allocation_requirement_bytes) ||
                table_owner_bytes == 0u || table_owner_bytes > table_owner_requirement ||
                index->table_allocation_bytes() != table_plan.allocation_bytes ||
                index->table_peak_allocation_bytes() > table_plan.allocation_bytes ||
                retained_index == (std::numeric_limits<std::uint64_t>::max)() ||
                retained_index > index_permit.bytes()) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                emit_transient_mpf_failure(
                    "index_reconcile_failed",
                    static_cast<std::uint64_t>(total_bp_size),
                    event_count,
                    index_peak,
                    index_permit.bytes(),
                    position_bytes,
                    retained_positions,
                    table_owner_requirement,
                    table_owner_bytes,
                    table_plan.allocation_bytes,
                    index->table_allocation_bytes(),
                    index->table_peak_allocation_bytes(),
                    shared_owner_bytes);
#endif
                raise_typed_dispatch_pg_error_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "prepared transient MPF index exceeded its physical plan");
            }
            index_permit.shrink_to(retained_index);
            result.permit = std::move(index_permit);
            result.index = std::move(index);
        });
    storage.release_runs();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    result.receipt = std::move(spill_receipt);
#endif
    return result;
}

/** Build or reuse one exact query-filtered MPF target for a prepared statement group. */
template <sequence_alphabet p_alphabet, class p_query, class p_group, class p_prepared_target>
[[nodiscard]] match_result_collection build_and_align_prepared_transient_minimizer_fragment(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    pipe::statement_execution_plan_owner execution_plan,
    sequence_size_t total_bp_size,
    std::uint8_t alphabet_code,
    std::uint8_t target_storage_unit_byte_width,
    const p_query& query,
    p_group& group,
    p_prepared_target& prepared_target) {
    if (!execution_plan || execution_plan->uses_vulkan_seed() ||
        total_bp_size != target_view.bp_size() ||
        alphabet_code != bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>() ||
        (target_storage_unit_byte_width != sizeof(std::uint32_t) &&
         target_storage_unit_byte_width != sizeof(std::uint64_t))) {
        throw std::invalid_argument{"prepared transient MPF target contract is invalid"};
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (params.max_occ < 0) {
        raise_transient_generation_error(
            "prepared transient MPF max-occ",
            bioseq::indexing::minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto target_generation = select_transient_target_generation<p_alphabet>(
        params, static_cast<std::uint64_t>(total_bp_size), target_storage_unit_byte_width);
    if (!query.minimizers.valid() ||
        !transient_query_layout_matches_target_identity(group.execution_layout,
                                                        target_generation.identity) ||
        query.minimizers.layout() != group.execution_layout) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "prepared transient MPF query and target generated identities disagree");
    }
    const bool build_target = group.target_index == nullptr;
    if (build_target) {
        if (group.transient_grouped_soa_reader.has_value() ||
            group.target_index_permit.has_value() || group.target_index_builds != 0u ||
            group.target_index_reuses != 0u || group.persisted_reader_loads != 0u
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            || group.transient_target_generation_receipt.has_value() ||
            group.transient_target_generation_stats_published
#endif
            ) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPF group already owns another target representation");
        }
        const auto canonical_hashes = group.canonical_query_hash_view();
        auto target_owner = build_admitted_transient_mpf_index(
            target_view,
            params,
            target_generation,
            total_bp_size,
            alphabet_code,
            canonical_hashes);
        group.target_index_permit.emplace(std::move(target_owner.permit));
        group.target_index = std::move(target_owner.index);
        if (!prepared_target.release_group_canonical_query_hashes_noexcept(group)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPF could not release its consumed query union");
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        group.transient_target_generation_receipt.emplace(std::move(target_owner.receipt));
#endif
        group.target_index_source = ::bioseq::pipeline_stats::target_index_source::recomputed;
        group.target_index_builds = 1u;
        group.build_set_checksum = group.key_set_checksum;
        group.build_set_checksum_present = true;
        ++prepared_target.physical_target_index_reads;
    } else {
        if (group.transient_grouped_soa_reader.has_value() ||
            !group.target_index_permit.has_value() || group.target_index_builds != 1u
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            || !group.transient_target_generation_receipt.has_value() ||
            !group.transient_target_generation_receipt->is_spill_valid()
#endif
            ||
            group.target_index_source !=
                ::bioseq::pipeline_stats::target_index_source::recomputed ||
            !group.build_set_checksum_present ||
            group.build_set_checksum != group.key_set_checksum) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPF retained target ownership is inconsistent");
        }
        ++group.target_index_reuses;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    try {
        return minimap_port_fragment_index_physical_pg_checked(
            query_view,
            target_view,
            query.minimizers,
            *group.target_index,
            execution_plan,
            /*gpu_index_cache=*/nullptr,
            bioseq::indexing::gpu_index_cache_identity{
                .assembly_identity = group.key_set_checksum},
            /*allow_gpu_target_index_build=*/false);
    } catch (const std::bad_alloc&) {
        emit_transient_mpf_failure(
            "prepared_downstream_alignment_after_mpf_ready",
            static_cast<std::uint64_t>(total_bp_size),
            static_cast<std::uint64_t>(group.target_index->positions.size()),
            0u,
            group.target_index_permit->bytes());
        throw;
    }
#else
    return minimap_port_fragment_index_physical_pg_checked(
        query_view,
        target_view,
        query.minimizers,
        *group.target_index,
        execution_plan,
        /*gpu_index_cache=*/nullptr,
        bioseq::indexing::gpu_index_cache_identity{
            .assembly_identity = group.key_set_checksum},
        /*allow_gpu_target_index_build=*/false);
#endif
}

/** Build or reuse one authenticated grouped-SoA target for a prepared statement group. */
template <sequence_alphabet p_alphabet, class p_query, class p_group, class p_prepared_target>
[[nodiscard]] match_result_collection build_and_align_prepared_transient_minimizer_sequence(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    pipe::statement_execution_plan_owner execution_plan,
    sequence_size_t total_bp_size,
    std::uint8_t alphabet_code,
    std::uint8_t target_storage_unit_byte_width,
    std::uint64_t source_epoch,
    const p_query& query,
    p_group& group,
    p_prepared_target& prepared_target) {
    if (!execution_plan || execution_plan->uses_vulkan_seed() || source_epoch == 0u ||
        total_bp_size != target_view.bp_size() ||
        alphabet_code != bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>() ||
        (target_storage_unit_byte_width != sizeof(std::uint32_t) &&
         target_storage_unit_byte_width != sizeof(std::uint64_t))) {
        throw std::invalid_argument{"prepared transient MPS target contract is invalid"};
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (params.max_occ < 0 ||
        static_cast<std::uint64_t>(params.max_occ) >
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap) {
        raise_transient_generation_error(
            "prepared transient MPS max-occ",
            bioseq::indexing::minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto target_generation = select_transient_target_generation<p_alphabet>(
        params, static_cast<std::uint64_t>(total_bp_size), target_storage_unit_byte_width);
    if (!query.minimizers.valid() ||
        !transient_query_layout_matches_target_identity(group.execution_layout,
                                                        target_generation.identity) ||
        query.minimizers.layout() != group.execution_layout) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "prepared transient MPS query and target generated identities disagree");
    }
    const bool build_target = !group.transient_grouped_soa_reader.has_value();
    if (build_target) {
        if (group.target_index != nullptr || group.target_index_permit.has_value() ||
            group.target_index_builds != 0u || group.target_index_reuses != 0u ||
            group.persisted_reader_loads != 0u
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            || group.transient_target_generation_receipt.has_value() ||
            group.transient_target_generation_stats_published
#endif
            ) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPS group already owns another target representation");
        }
        opportunistic_sequence_index_storage storage;
        storage.run_buffer_bytes_value =
            bioseq::settings::index_build::run_buffer_bytes > 0
                ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
                : (std::size_t{256} << 20);
        bioseq::indexing::minimizer_sequence_generation_execution_receipt receipt{};
        using sequence_index =
            bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>;
        auto spill = spill_transient_target_for_prepared_queries(
            storage,
            target_view,
            params,
            target_generation,
            alphabet_code,
            group.canonical_query_hash_view(),
            receipt);
        if (!spill || !receipt.is_spill_valid()) {
            raise_transient_generation_error(
                "prepared transient MPS spill",
                spill ? bioseq::indexing::minimizer_generation_encoding_error::replay_mismatch
                      : spill.error());
        }
        const auto scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(
            target_generation.backend);
        const bioseq::indexing::grouped_soa_generation_options options{
            .layout = bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local,
            .generation_id = source_epoch,
            .source_identity = source_epoch,
            .source_content_checksum = 0u,
            .target_length = static_cast<std::uint64_t>(total_bp_size),
            .position_segment_span_bp =
                bioseq::indexing::k_minimizer_preferred_segment_span_bp,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = static_cast<std::uint16_t>(
                target_generation.identity.packed_storage_bits / 8u),
            .kmer_size = static_cast<std::uint16_t>(params.kmer_size),
            .window_size = static_cast<std::uint16_t>(params.window_size),
            .minimizer_scheme_id = scheme.id,
            .minimizer_scheme_revision = scheme.revision,
            .algorithm_identity = target_generation.identity,
            .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
        };
        transient_grouped_soa_payload_storage payload;
        auto encoded = sequence_index::encode_persisted_generation(
            storage,
            payload,
            std::move(*spill),
            options,
            bioseq::indexing::grouped_soa_generation_limits{},
            std::addressof(receipt));
        if (!encoded) {
            raise_transient_generation_error("prepared transient MPS encode", encoded.error());
        }
        const auto publication =
            bioseq::indexing::make_minimizer_generation_publication_binding(*encoded);
        const auto spill_binding = storage.segment_spill_storage_binding();
        if (!receipt.matches_publication(publication) ||
            !receipt.allows_ready_release(spill_binding)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPS receipt does not authorize its payload");
        }
        if (auto complete = payload.complete(*encoded); !complete) {
            raise_transient_generation_error("prepared transient MPS payload", complete.error());
        }
        storage.release_runs();
        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
                target_generation.backend,
                target_generation.identity,
                alphabet_code,
                static_cast<std::uint16_t>(
                    target_generation.identity.packed_storage_bits / 8u),
                static_cast<std::uint16_t>(params.kmer_size),
                static_cast<std::uint16_t>(params.window_size),
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            raise_transient_generation_error(
                "prepared transient MPS expected identity", expected.error());
        }
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        const auto canonical_hashes = group.canonical_query_hash_view();
        std::expected<bioseq::indexing::prepared_grouped_soa_query_reader,
                      bioseq::indexing::minimizer_generation_encoding_error>
            prepared = canonical_hashes.width_bytes == sizeof(std::uint32_t)
                           ? bioseq::indexing::prepared_grouped_soa_query_reader::begin(
                                 admission,
                                 encoded->head,
                                 encoded->manifest,
                                 *expected,
                                 encoded->directory.encoded_bytes(),
                                 canonical_hashes.values32,
                                 static_cast<std::uint64_t>(params.max_occ))
                           : bioseq::indexing::prepared_grouped_soa_query_reader::begin(
                                 admission,
                                 encoded->head,
                                 encoded->manifest,
                                 *expected,
                                 encoded->directory.encoded_bytes(),
                                 canonical_hashes.values64,
                                 static_cast<std::uint64_t>(params.max_occ));
        if (!prepared) {
            raise_transient_generation_error(
                "prepared transient MPS reader begin", prepared.error());
        }
        auto replayed = payload.for_each_payload_row(
            [&reader = *prepared](const bioseq::indexing::minimizer_generation_row_view& row)
                -> std::expected<void,
                                 bioseq::indexing::minimizer_generation_encoding_error> {
                auto authenticated =
                    bioseq::indexing::validate_minimizer_generation_row_receipt(row);
                if (!authenticated) {
                    return std::unexpected(authenticated.error());
                }
                return reader.accept(*authenticated);
            });
        if (!replayed) {
            raise_transient_generation_error(
                "prepared transient MPS reader replay", replayed.error());
        }
        if (auto finished = prepared->finish(); !finished) {
            raise_transient_generation_error(
                "prepared transient MPS reader finish", finished.error());
        }
        group.transient_grouped_soa_reader.emplace(std::move(*prepared));
        if (!prepared_target.release_group_canonical_query_hashes_noexcept(group)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPS could not release its consumed query union");
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        group.transient_target_generation_receipt.emplace(receipt);
#endif
        group.target_index_source = ::bioseq::pipeline_stats::target_index_source::recomputed;
        group.target_index_builds = 1u;
        group.build_set_checksum = group.key_set_checksum;
        group.build_set_checksum_present = true;
        ++prepared_target.physical_target_index_reads;
    } else {
        if (group.target_index != nullptr || group.target_index_permit.has_value() ||
            !group.transient_grouped_soa_reader->ready()
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            || !group.transient_target_generation_receipt.has_value() ||
            !group.transient_target_generation_receipt->is_internally_consistent()
#endif
            ||
            group.target_index_builds != 1u ||
            group.target_index_source !=
                ::bioseq::pipeline_stats::target_index_source::recomputed ||
            !group.build_set_checksum_present ||
            group.build_set_checksum != group.key_set_checksum) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "prepared transient MPS retained target ownership is inconsistent");
        }
        ++group.target_index_reuses;
    }
    return run_aligner_pg_checked(
        "prepared transient grouped-SoA minimizer-sequence query",
        [query_view,
         target_view,
         &query,
         &reader = *group.transient_grouped_soa_reader,
         execution_plan] {
            return minimap_port_grouped_soa_owned(
                query_view,
                target_view,
                query.minimizers,
                reader,
                execution_plan,
                /*gpu_index_cache=*/nullptr,
                bioseq::indexing::gpu_index_cache_identity{},
                /*allow_gpu_target_index_build=*/false);
        });
}

/** Build and align a transient MPF target without any persisted-index read. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection build_and_align_transient_minimizer_fragment(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    pipe::statement_execution_plan_owner execution_plan,
    sequence_size_t total_bp_size,
    std::size_t target_letters_per_unit,
    std::uint8_t alphabet_code,
    std::uint8_t query_storage_unit_byte_width,
    std::uint8_t target_storage_unit_byte_width) {
    if (!execution_plan) {
        throw std::invalid_argument{"transient fragment operation requires a statement plan"};
    }
    const std::size_t expected_target_letters_per_unit =
        target_storage_unit_byte_width * std::numeric_limits<unsigned char>::digits /
        bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol;
    if (total_bp_size != target_view.bp_size() || target_letters_per_unit == 0u ||
        target_letters_per_unit != expected_target_letters_per_unit ||
        alphabet_code !=
            bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient MPF source length or packed geometry is inconsistent");
    }
    if (execution_plan->uses_vulkan_seed()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient MPF Vulkan seed requires the external GPU target owner");
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (params.max_occ < 0) {
        raise_transient_generation_error(
            "transient MPF max-occ",
            bioseq::indexing::minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    transient_physical_query query = prepare_transient_physical_query(
        query_view, params, query_storage_unit_byte_width, true);
    const auto target_generation = select_transient_target_generation<p_alphabet>(
        params, static_cast<std::uint64_t>(total_bp_size), target_storage_unit_byte_width);
    const auto& target_identity = target_generation.identity;
    if (!transient_query_matches_target_identity(query, target_identity)) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient MPF query and target selected incompatible generated records");
    }
    std::uint8_t canonical_hash_storage_width = 0u;
    if (query.layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u32) {
        canonical_hash_storage_width = static_cast<std::uint8_t>(sizeof(std::uint32_t));
    } else if (
        query.layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u64 ||
        (bioseq::minimizers::minimizer_physical_record_detail::k_has_32_bit_record &&
         query.layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u16)) {
        canonical_hash_storage_width = static_cast<std::uint8_t>(sizeof(std::uint64_t));
    } else {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient MPF query selected an unsupported canonical hash width");
    }
    const bioseq::indexing::canonical_query_hash_view canonical_hashes{
        .values32 = std::span<const std::uint32_t>{query.hashes_u32},
        .values64 = std::span<const std::uint64_t>{query.hashes_u64},
        .width_bytes = canonical_hash_storage_width,
        .storage_width_bytes = canonical_hash_storage_width,
    };
    auto target_owner = build_admitted_transient_mpf_index(
        target_view,
        params,
        target_generation,
        total_bp_size,
        alphabet_code,
        canonical_hashes);

    const std::uint64_t query_minimizer_bytes = query.minimizers.capacity_bytes();
    const std::uint64_t query_u32_hash_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(query.hashes_u32.capacity()),
        static_cast<std::uint64_t>(sizeof(std::uint32_t)));
    const std::uint64_t query_u64_hash_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(query.hashes_u64.capacity()),
        static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    const std::uint64_t query_retained_bytes = bioseq::common::saturating_add(
        query_minimizer_bytes,
        bioseq::common::saturating_add(query_u32_hash_bytes, query_u64_hash_bytes));
    if (query_retained_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        query_retained_bytes != query.permit.bytes()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient MPF query ownership changed before canonical hash release");
    }
    std::vector<std::uint32_t>{}.swap(query.hashes_u32);
    std::vector<std::uint64_t>{}.swap(query.hashes_u64);
    query.permit.shrink_to(query.minimizers.capacity_bytes());
    if (query.permit.bytes() != query_minimizer_bytes) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient MPF query permit did not reconcile after canonical hash release");
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    match_result_collection collection = [&]() {
        try {
            return minimap_port_fragment_index_physical_pg_checked(
                query_view,
                target_view,
                query.minimizers,
                *target_owner.index,
                execution_plan,
                /*gpu_index_cache=*/nullptr,
                bioseq::indexing::gpu_index_cache_identity{},
                /*allow_gpu_target_index_build=*/false);
        } catch (const std::bad_alloc&) {
            emit_transient_mpf_failure(
                "ordinary_downstream_alignment_after_mpf_build",
                static_cast<std::uint64_t>(total_bp_size),
                static_cast<std::uint64_t>(target_owner.index->positions.size()),
                0u,
                target_owner.permit.bytes());
            throw;
        }
    }();
#else
    match_result_collection collection = minimap_port_fragment_index_physical_pg_checked(
        query_view,
        target_view,
        query.minimizers,
        *target_owner.index,
        execution_plan,
        /*gpu_index_cache=*/nullptr,
        bioseq::indexing::gpu_index_cache_identity{},
        /*allow_gpu_target_index_build=*/false);
#endif
    record_transient_query_execution(collection, query);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (target_generation.backend == minimizer_sketch_backend::bounded_nthash_simd) {
        record_transient_bounded_target_execution(
            collection, target_owner.receipt.packed_bounded_stats);
    } else if (target_generation.backend == minimizer_sketch_backend::scalar ||
               target_generation.backend == minimizer_sketch_backend::exact_simd) {
        record_transient_cpu_target_generation_authority(
            collection,
            transient_cpu_target_generation_representation::minimizer_fragment_spill,
            std::move(target_owner.receipt));
    }
#endif
    return collection;
}

/** Build an authenticated transient grouped-SoA MPS target and align through its reader. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection build_and_align_transient_minimizer_sequence(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    pipe::statement_execution_plan_owner execution_plan,
    sequence_size_t total_bp_size,
    std::size_t target_letters_per_unit,
    std::uint8_t alphabet_code,
    std::uint8_t query_storage_unit_byte_width,
    std::uint8_t target_storage_unit_byte_width,
    std::uint64_t source_epoch) {
    if (!execution_plan || source_epoch == 0u) {
        throw std::invalid_argument{
            "transient minimizer-sequence operation requires plan and source identity"};
    }
    const std::size_t expected_target_letters_per_unit =
        target_storage_unit_byte_width * std::numeric_limits<unsigned char>::digits /
        bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol;
    if (total_bp_size != target_view.bp_size() || target_letters_per_unit == 0u ||
        target_letters_per_unit != expected_target_letters_per_unit ||
        alphabet_code !=
            bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient MPS source length or packed geometry is inconsistent");
    }
    if (execution_plan->uses_vulkan_seed()) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient MPS Vulkan seed requires the external GPU target owner");
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (params.max_occ < 0 ||
        static_cast<std::uint64_t>(params.max_occ) >
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap) {
        raise_transient_generation_error(
            "transient MPS max-occ",
            bioseq::indexing::minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    transient_physical_query query = prepare_transient_physical_query(
        query_view, params, query_storage_unit_byte_width, true);
    const auto target_generation = select_transient_target_generation<p_alphabet>(
            params,
            static_cast<std::uint64_t>(total_bp_size),
            target_storage_unit_byte_width);
    const auto& target_identity = target_generation.identity;
    const minimizer_sketch_backend target_backend = target_generation.backend;
    if (!transient_query_matches_target_identity(query, target_identity)) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "transient MPS query and target selected incompatible generated records");
    }
    const auto* kernels = params.highway_kernels == nullptr
                              ? nullptr
                              : std::addressof(params.highway_kernels->minimizers);
    opportunistic_sequence_index_storage storage;
    storage.run_buffer_bytes_value =
        bioseq::settings::index_build::run_buffer_bytes > 0
            ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
            : (std::size_t{256} << 20);
    bioseq::indexing::minimizer_sequence_generation_execution_receipt receipt{};
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>;
    auto spill = sequence_index::spill_generation_replay_runs_serial(
        storage,
        target_view,
        target_backend,
        kernels,
        target_identity,
        /*core_span_bp=*/0u,
        bioseq::indexing::k_minimizer_preferred_segment_span_bp,
        std::addressof(receipt),
        alphabet_code);
    if (!spill) {
        raise_transient_generation_error("transient MPS spill", spill.error());
    }
    const auto scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(target_backend);
    const bioseq::indexing::grouped_soa_generation_options options{
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
        .generation_id = source_epoch,
        .source_identity = source_epoch,
        .source_content_checksum = 0u,
        .target_length = static_cast<std::uint64_t>(total_bp_size),
        .position_segment_span_bp =
            bioseq::indexing::k_minimizer_preferred_segment_span_bp,
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width =
            static_cast<std::uint16_t>(target_identity.packed_storage_bits / 8u),
        .kmer_size = static_cast<std::uint16_t>(params.kmer_size),
        .window_size = static_cast<std::uint16_t>(params.window_size),
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .algorithm_identity = target_identity,
        .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
    };
    transient_grouped_soa_payload_storage payload;
    auto encoded = sequence_index::encode_persisted_generation(
        storage,
        payload,
        std::move(*spill),
        options,
        bioseq::indexing::grouped_soa_generation_limits{},
        std::addressof(receipt));
    if (!encoded) {
        raise_transient_generation_error("transient MPS encode", encoded.error());
    }
    const auto publication =
        bioseq::indexing::make_minimizer_generation_publication_binding(*encoded);
    const auto spill_binding = storage.segment_spill_storage_binding();
    if (!receipt.matches_publication(publication) ||
        !receipt.allows_ready_release(spill_binding)) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "transient MPS execution receipt does not authorize its encoded payload");
    }
    if (auto complete = payload.complete(*encoded); !complete) {
        raise_transient_generation_error("transient MPS payload", complete.error());
    }
    storage.release_runs();

    const auto expected =
        bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
            bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
            target_backend,
            target_identity,
            alphabet_code,
            static_cast<std::uint16_t>(target_identity.packed_storage_bits / 8u),
            static_cast<std::uint16_t>(params.kmer_size),
            static_cast<std::uint16_t>(params.window_size),
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
    if (!expected) {
        raise_transient_generation_error("transient MPS expected identity", expected.error());
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    std::expected<bioseq::indexing::prepared_grouped_soa_query_reader,
                  bioseq::indexing::minimizer_generation_encoding_error>
        prepared = query.layout.hash_width == bioseq::minimizers::minimizer_scalar_width::u32
            ? bioseq::indexing::prepared_grouped_soa_query_reader::begin(
                  admission,
                  encoded->head,
                  encoded->manifest,
                  *expected,
                  encoded->directory.encoded_bytes(),
                  std::span<const std::uint32_t>{query.hashes_u32},
                  static_cast<std::uint64_t>(params.max_occ))
            : bioseq::indexing::prepared_grouped_soa_query_reader::begin(
                  admission,
                  encoded->head,
                  encoded->manifest,
                  *expected,
                  encoded->directory.encoded_bytes(),
                  std::span<const std::uint64_t>{query.hashes_u64},
                  static_cast<std::uint64_t>(params.max_occ));
    if (!prepared) {
        raise_transient_generation_error("transient MPS reader begin", prepared.error());
    }
    auto replayed = payload.for_each_payload_row(
        [&reader = *prepared](const bioseq::indexing::minimizer_generation_row_view& row)
            -> std::expected<void, bioseq::indexing::minimizer_generation_encoding_error> {
            auto authenticated =
                bioseq::indexing::validate_minimizer_generation_row_receipt(row);
            if (!authenticated) {
                return std::unexpected(authenticated.error());
            }
            return reader.accept(*authenticated);
        });
    if (!replayed) {
        raise_transient_generation_error("transient MPS reader replay", replayed.error());
    }
    if (auto finished = prepared->finish(); !finished) {
        raise_transient_generation_error("transient MPS reader finish", finished.error());
    }
    match_result_collection collection = run_aligner_pg_checked(
        "transient grouped-SoA minimizer-sequence query",
        [query_view, target_view, &query, &reader = *prepared, execution_plan] {
            return minimap_port_grouped_soa_owned(query_view,
                                                  target_view,
                                                  query.minimizers,
                                                  reader,
                                                  execution_plan,
                                                  /*gpu_index_cache=*/nullptr,
                                                  bioseq::indexing::gpu_index_cache_identity{},
                                                  /*allow_gpu_target_index_build=*/false);
        });
    record_transient_query_execution(collection, query);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (target_backend == minimizer_sketch_backend::bounded_nthash_simd) {
        record_transient_bounded_target_execution(collection, receipt.packed_bounded_stats);
    } else if (target_backend == minimizer_sketch_backend::scalar ||
               target_backend == minimizer_sketch_backend::exact_simd) {
        record_transient_cpu_target_generation_authority(
            collection,
            transient_cpu_target_generation_representation::minimizer_sequence_publication,
            std::move(receipt));
    }
#endif
    return collection;
}

template <class p_scanner_pg_context>
[[nodiscard]] bool
stage_recompute_minimizer_sequence_build_if_selected(const p_scanner_pg_context& ctx,
                                                     const bioseq::storage::sequence_locator& loc,
                                                     std::uint64_t sequence_rowno,
                                                     std::uint8_t alphabet_code,
                                                     std::uint8_t storage_unit_byte_width,
                                                     std::uint16_t last_fragment_padding,
                                                     pipe::statement_execution_plan_owner execution_plan,
                                                     std::string_view reason) {
    if (!execution_plan) {
        throw std::invalid_argument("transient index operation requires a statement plan owner");
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (!can_defer_recompute_minimizer_persist(ctx, params)) {
        if (ctx.recompute_async_persist_active &&
            target_backend_from_params_for_recompute(params) ==
                bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} because persistence requires a scheme-tagged READY "
                "generation; recompute continues without fallback",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason);
        } else if (ctx.recompute_async_persist_active &&
                   (params.kmer_size != 15 || params.window_size != 10)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} because active params are k={} w={} but persisted "
                "minimizer indexes are pinned to k=15 w=10",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason,
                params.kmer_size,
                params.window_size);
        } else if (!ctx.recompute_async_persist_active || !ctx.index_async_persist_active ||
                   !OidIsValid(ctx.user_relid)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} gates recompute_async={} index_async={} "
                "user_relid_valid={}",
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
        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} "
            "reason={} because no minimizer index is enabled for UPDATE_ON_INSERT",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(loc.logical_length),
            reason);
        return false;
    }
    if (choice->kind != deferred_minimizer_index_kind::per_sequence) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute async minimizer-sequence build skipped row={} index={} "
            "target_bp={} reason={} because deferred choice is not per-sequence",
            static_cast<unsigned long long>(sequence_rowno),
            choice->index_name,
            static_cast<unsigned long long>(loc.logical_length),
            reason);
        return false;
    }

    // The legacy async request carries only a backend enum. Admit it only when that enum is a
    // lossless encoding of the generated requested identity; reference identities require the
    // newer plan-owned publisher and must not be relabelled as scalar or two-stack work.
    if (storage_unit_byte_width != sizeof(std::uint32_t) &&
        storage_unit_byte_width != sizeof(std::uint64_t)) {
        return false;
    }
    const auto generation_identity =
        bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            params.minimizer_algorithm,
            loc.logical_length,
            alphabet_code,
            static_cast<std::uint8_t>(storage_unit_byte_width * 8u),
            static_cast<std::uint16_t>(params.kmer_size),
            static_cast<std::uint16_t>(params.window_size));
    const auto effective_backend =
        generation_identity
            ? bioseq::indexing::minimizer_sketch_backend_for_effective_algorithm(
                  generation_identity->effective_algorithm)
            : std::optional<minimizer_sketch_backend>{};
    const auto transport_algorithm =
        effective_backend
            ? bioseq::indexing::minimizer_algorithm_for_explicit_sketch_backend(
                  *effective_backend)
            : std::optional<minimizer_algorithm>{};
    if (!generation_identity || !effective_backend || !transport_algorithm ||
        *transport_algorithm != generation_identity->requested_algorithm) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: async minimizer-sequence build skipped row={} target_bp={} "
            "reason={} because the legacy worker request cannot carry the generated identity",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(loc.logical_length),
            reason);
        return false;
    }

    const auto persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, choice->index_name);
    const bool staged = bioseq::postgres::scheduler::stage_async_minimizer_sequence_build(
        persist_target,
        loc,
        alphabet_code,
        storage_unit_byte_width,
        last_fragment_padding,
        static_cast<std::int32_t>(
            bioseq::settings::index::sequence_minimizer_sort::algorithm.get_value()),
        static_cast<int>(bioseq::scheduler::settings::pipeline::mode.get_value()),
        bioseq::scheduler::settings::backend::max_concurrency,
        bioseq::settings::index::memory_cap_mb,
        static_cast<std::int32_t>(*effective_backend),
        params.highway_executed_target);
    bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build "
                                     "row={} index={} target_bp={} reason={} async_staged={}",
                                     static_cast<unsigned long long>(sequence_rowno),
                                     choice->index_name,
                                     static_cast<unsigned long long>(loc.logical_length),
                                     reason,
                                     staged);
    return staged;
}

template <sequence_alphabet p_alphabet, class p_scanner_pg_context>
[[nodiscard]] match_result_collection run_recompute_with_deferred_minimizer_persist(
    bioseq::storage::lazy_sequence_view<p_alphabet> lazy_query,
    bioseq::storage::lazy_sequence_view<p_alphabet> resident_target,
    pipe::statement_execution_plan_owner execution_plan,
    const p_scanner_pg_context& ctx,
    const bioseq::storage::sequence_locator& loc,
    std::uint64_t sequence_rowno,
    std::uint8_t alphabet_code,
    std::uint8_t query_storage_unit_byte_width,
    std::uint8_t target_storage_unit_byte_width,
    std::uint16_t last_fragment_padding,
    const deferred_minimizer_persist_choice& choice) {
    if (!execution_plan) {
        throw std::invalid_argument("transient index operation requires a statement plan owner");
    }
    if (choice.kind == deferred_minimizer_index_kind::per_sequence) {
        // Real minimizer-sequence no-index route: build a transient minimizers_per_sequence
        // index through the generated host producer, authenticate its reader, and run the
        // indexed suffix. The external GPU target owner is deliberately not synthesized here.
        (void)ctx;
        (void)last_fragment_padding;
        match_result_collection collection = build_and_align_transient_minimizer_sequence(
            lazy_query,
            resident_target,
            execution_plan,
            static_cast<sequence_size_t>(loc.logical_length),
            target_storage_unit_byte_width * std::numeric_limits<unsigned char>::digits /
                bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol,
            alphabet_code,
            query_storage_unit_byte_width,
            target_storage_unit_byte_width,
            loc.source_epoch);

        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute real-minimizer-sequence-noindex row={} index={} "
            "target_bp={} persisted=false",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.logical_length));
        return collection;
    }

    (void)ctx;
    (void)last_fragment_padding;
    match_result_collection collection = build_and_align_transient_minimizer_fragment(
        lazy_query,
        resident_target,
        execution_plan,
        static_cast<sequence_size_t>(loc.logical_length),
        target_storage_unit_byte_width * std::numeric_limits<unsigned char>::digits /
            bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol,
        alphabet_code,
        query_storage_unit_byte_width,
        target_storage_unit_byte_width);
    bioseq_log<logging_level::debug>(
        "[bioseq] index: recompute transient-minimizer-fragment-noindex row={} index={} "
        "target_bp={} persisted=false external_constructor_entries={} "
        "external_writer_entries={}",
        static_cast<unsigned long long>(sequence_rowno),
        choice.index_name,
        static_cast<unsigned long long>(loc.logical_length),
        static_cast<unsigned long long>(
            bioseq::storage::indexing::g_prohibited_external_file_producer_entry_counters
                .constructor_entries),
        static_cast<unsigned long long>(
            bioseq::storage::indexing::g_prohibited_external_file_producer_entry_counters
                .writer_entries));
    return collection;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
