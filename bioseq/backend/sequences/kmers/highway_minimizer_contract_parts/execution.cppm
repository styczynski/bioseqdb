/**
 * @file execution.cppm
 * @brief Minimizer backend statuses and execution counters.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_minimizer_contract:execution;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;
import :representation;
import :collectors;

export namespace bioseq::minimizers {

/** Runtime backend selected for one minimizer sketch operation. */
enum class minimizer_sketch_backend : std::uint8_t {
    scalar = 0,
    vulkan_segment = 1,
    exact_simd = 2,
    bounded_nthash_simd = 3,
};

/** @brief Returns the diagnostic name of a backend, or "unknown". */
[[nodiscard]] constexpr const char* minimizer_sketch_backend_name(
    const minimizer_sketch_backend backend) noexcept
{
    switch (backend) {
        case minimizer_sketch_backend::scalar: return "scalar";
        case minimizer_sketch_backend::vulkan_segment: return "vulkan_segment";
        case minimizer_sketch_backend::exact_simd:
            return "exact_simd";
        case minimizer_sketch_backend::bounded_nthash_simd:
            return "bounded_nthash_simd";
    }
    return "unknown";
}

/** @brief Checks whether the selected backend is vulkan_segment. */
[[nodiscard]] constexpr bool is_vulkan_minimizer_backend(
    const minimizer_sketch_backend backend) noexcept
{
    return backend == minimizer_sketch_backend::vulkan_segment;
}

/** A (position, hash) pair used for sorted minimizer output. */
using minimizer_sorted_pair =
    std::pair<std::uint64_t, std::uint64_t>;

/** Status of an incremental two-stack minimizer execution. */
enum class two_stack_continuation_status : std::uint8_t {
    ok = 0,
    invalid_window_size,
    invalid_kmer_size,
    invalid_static_configuration,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    not_initialized,
    already_finalized,
    invalid_state,
    invalid_range,
    non_contiguous_range,
    arithmetic_overflow,
    collector_overflow,
};

/** Two-stack implementation recorded for the executed kernel. */
enum class exact_simd_kernel : std::uint8_t {
    generic_u64 = 0,
    kmer_15_window_10_byte_8_uint32_hash,
};

/** @brief Returns the diagnostic name of a continuation status, or "unknown". */
[[nodiscard]] constexpr std::string_view two_stack_continuation_status_name(
    two_stack_continuation_status status) noexcept
{
    switch (status) {
        case two_stack_continuation_status::ok: return "ok";
        case two_stack_continuation_status::invalid_window_size:
            return "invalid_window_size";
        case two_stack_continuation_status::invalid_kmer_size:
            return "invalid_kmer_size";
        case two_stack_continuation_status::invalid_static_configuration:
            return "invalid_static_configuration";
        case two_stack_continuation_status::unsupported_alphabet:
            return "unsupported_alphabet";
        case two_stack_continuation_status::unsupported_storage:
            return "unsupported_storage";
        case two_stack_continuation_status::unsupported_representation:
            return "unsupported_representation";
        case two_stack_continuation_status::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case two_stack_continuation_status::not_initialized:
            return "not_initialized";
        case two_stack_continuation_status::already_finalized:
            return "already_finalized";
        case two_stack_continuation_status::invalid_state:
            return "invalid_state";
        case two_stack_continuation_status::invalid_range:
            return "invalid_range";
        case two_stack_continuation_status::non_contiguous_range:
            return "non_contiguous_range";
        case two_stack_continuation_status::arithmetic_overflow:
            return "arithmetic_overflow";
        case two_stack_continuation_status::collector_overflow:
            return "collector_overflow";
    }
    return "unknown";
}

/** @brief Reports a rejected two-stack SIMD operation with its status code. */
class exact_simd_runtime_error final : public std::runtime_error {
public:
    /** @brief Builds an error message from the operation name and continuation status. */
    exact_simd_runtime_error(
        two_stack_continuation_status status,
        std::string_view operation)
        : std::runtime_error(
              std::string{operation}
              + ": two_stack_nthash minimizer execution rejected: "
              + std::string{two_stack_continuation_status_name(status)}),
          status_(status) {}

    /** @brief Returns the continuation error code. */
    [[nodiscard]] two_stack_continuation_status status() const noexcept {
        return status_;
    }

private:
    two_stack_continuation_status status_;
};

/** Work counters for the two-stack minimizer algorithms. */
struct two_stack_execution_stats {
    std::uint64_t bp_consumed = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t vector_lanes_visited = 0;
    std::uint64_t masked_tail_lanes = 0;
    std::uint64_t bridge_symbols_staged = 0;
    std::uint64_t bridge_vector_runs = 0;
    std::uint64_t consecutive_kmer_batches = 0;
    std::uint64_t canonical_scramble_batches = 0;
    std::uint64_t window_reduction_batches = 0;
    std::uint64_t event_generation_batches = 0;
    std::uint64_t stable_compress_batches = 0;
    std::uint64_t direct_event_compress_batches = 0;
    std::uint64_t tie_matrix_event_batches = 0;
    std::uint64_t row_transpose_batches = 0;
    std::uint64_t row_vector_stores = 0;
    std::uint64_t palindrome_lanes = 0;
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    std::uint64_t tie_hash_prefilter_batches = 0;
    std::uint64_t tie_hash_prefilter_rejected_lanes = 0;
    std::uint64_t tie_raw_verification_batches = 0;
    std::uint64_t entering_min_transitions = 0;
    std::uint64_t expiry_rescan_transitions = 0;
    std::uint64_t final_flush_records = 0;
    std::uint64_t fragment_symbols_staged = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lanes = 0;
    std::uint64_t packed_word_boundary_merges = 0;
    std::uint64_t ambiguous_symbols = 0;
    std::uint64_t ambiguous_reset_batches = 0;
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_backend_branches = 0;
    std::uint64_t scalar_tail_symbols = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_event_records = 0;
    std::uint64_t scalar_sort_records = 0;
    std::uint64_t scalar_dedup_records = 0;
    // Count scalable-vector storage fallbacks separately from fixed-width SIMD work.
    std::uint64_t scalable_storage_fallback_calls = 0;
    std::uint64_t scalable_storage_fallback_input_symbols = 0;
    std::uint64_t scalable_storage_fallback_minimizers_emitted = 0;
    // These counters record calls that scan a complete DNA4/u32 sequence
    // through the vector-stream collector. Continuation calls use their
    // own counters, allowing the checker to detect mixed execution paths.
    std::uint64_t resident_vector_stream_kernel_calls = 0;
    std::uint64_t resident_vector_stream_single_calls = 0;
    std::uint64_t resident_vector_stream_dual_calls = 0;
    std::uint64_t resident_vector_stream_nonempty_work_calls = 0;
    std::uint64_t resident_vector_stream_input_bp = 0;
    std::uint64_t resident_vector_stream_minimizers_emitted = 0;
    std::uint64_t generic_u64_kernel_calls = 0;
    std::uint64_t generic_u64_candidate_blocks = 0;
    std::uint64_t generic_u64_lane_visits = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_candidate_blocks = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_lane_visits = 0;
    std::uint64_t kmer_15_window_10_byte_8_packed_22_groups = 0;
    std::uint64_t kmer_15_window_10_byte_8_packed_22_lane_visits = 0;
    bool saturated = false;

    /**
     * @brief Adds a counter, stopping at UINT64_MAX on overflow.
     * did_saturate records an overflow and remains set once raised.
     */
    static void add_counter(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** @brief Adds another execution's counters and carries forward overflow flags. */
    void merge(const two_stack_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
#define BIOSEQ_MERGE_TWO_STACK_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bp_consumed);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(vector_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(vector_lanes_visited);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(masked_tail_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bridge_symbols_staged);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bridge_vector_runs);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(consecutive_kmer_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(canonical_scramble_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(window_reduction_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(event_generation_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(stable_compress_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(direct_event_compress_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_matrix_event_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(row_transpose_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(row_vector_stores);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(palindrome_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(first_window_tie_events);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(expiry_tie_events);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_hash_prefilter_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            tie_hash_prefilter_rejected_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_raw_verification_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(entering_min_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(expiry_rescan_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(final_flush_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(fragment_symbols_staged);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_decode_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_decode_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_word_boundary_merges);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(ambiguous_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(ambiguous_reset_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_orchestration_steps);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(fail_closed_rejections);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_backend_branches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_tail_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_hash_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_window_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_event_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_sort_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_dedup_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_input_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_single_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_dual_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_nonempty_work_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_input_bp);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_candidate_blocks);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_lane_visits);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_candidate_blocks);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_lane_visits);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(kmer_15_window_10_byte_8_packed_22_groups);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_packed_22_lane_visits);
#undef BIOSEQ_MERGE_TWO_STACK_COUNTER
    }

    /**
     * @brief Checks that counters describe one supported SIMD execution path.
     * Rejects overflow, fallback work and incompatible groups of counters.
     * require_nonempty_vector_work also requires actual SIMD work.
     */
    [[nodiscard]] constexpr bool is_internally_consistent(
            bool require_nonempty_vector_work = false) const noexcept {
        const std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        // Fixed-width SIMD statistics must contain no scalable-storage fallback work.
        if (scalable_storage_fallback_calls != 0
            || scalable_storage_fallback_input_symbols != 0
            || scalable_storage_fallback_minimizers_emitted != 0) {
            return false;
        }
        const bool resident_payload_present =
            resident_vector_stream_kernel_calls != 0
            || resident_vector_stream_single_calls != 0
            || resident_vector_stream_dual_calls != 0
            || resident_vector_stream_nonempty_work_calls != 0
            || resident_vector_stream_input_bp != 0
            || resident_vector_stream_minimizers_emitted != 0;
        if (resident_payload_present) {
            const bool stream_call_sum_valid =
                resident_vector_stream_single_calls
                    <= maximum - resident_vector_stream_dual_calls
                && resident_vector_stream_single_calls
                    + resident_vector_stream_dual_calls
                    == resident_vector_stream_kernel_calls;
            const bool continuation_payload_absent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && bridge_symbols_staged == 0
                && bridge_vector_runs == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && event_generation_batches == 0
                && stable_compress_batches == 0
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0
                && palindrome_lanes == 0
                && first_window_tie_events == 0
                && expiry_tie_events == 0
                && tie_hash_prefilter_batches == 0
                && tie_hash_prefilter_rejected_lanes == 0
                && tie_raw_verification_batches == 0
                && entering_min_transitions == 0
                && expiry_rescan_transitions == 0
                && final_flush_records == 0
                && fragment_symbols_staged == 0
                && packed_decode_batches == 0
                && packed_decode_lanes == 0
                && packed_word_boundary_merges == 0
                && ambiguous_symbols == 0
                && ambiguous_reset_batches == 0
                && scalar_orchestration_steps == 0
                && generic_u64_kernel_calls == 0
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0
                && kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == 0
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == 0
                && kmer_15_window_10_byte_8_packed_22_groups == 0
                && kmer_15_window_10_byte_8_packed_22_lane_visits == 0;
            const bool resident_backend = !saturated
                && resident_vector_stream_kernel_calls > 0
                && stream_call_sum_valid
                && resident_vector_stream_nonempty_work_calls
                    <= resident_vector_stream_kernel_calls
                && (resident_vector_stream_nonempty_work_calls == 0
                    || resident_vector_stream_minimizers_emitted > 0)
                && resident_vector_stream_input_bp == bp_consumed
                && resident_vector_stream_minimizers_emitted
                    == minimizers_emitted
                && fail_closed_rejections == 0
                && scalar_backend_branches == 0
                && scalar_tail_symbols == 0
                && scalar_hash_symbols == 0
                && scalar_window_transitions == 0
                && scalar_event_records == 0
                && scalar_sort_records == 0
                && scalar_dedup_records == 0
                && continuation_payload_absent;
            return resident_backend
                && (!require_nonempty_vector_work
                    || (bp_consumed > 0
                        && resident_vector_stream_nonempty_work_calls > 0));
        }
        const bool generic_selected = generic_u64_kernel_calls > 0
            && kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0;
        const bool fast_selected = generic_u64_kernel_calls == 0
            && kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0;
        const bool typed_route_complete =
            (generic_selected
                && generic_u64_candidate_blocks == vector_batches
                && generic_u64_lane_visits == vector_lanes_visited
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == 0
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == 0
                && kmer_15_window_10_byte_8_packed_22_groups == 0
                && kmer_15_window_10_byte_8_packed_22_lane_visits == 0)
            || (fast_selected
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == vector_batches
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == vector_lanes_visited
                && kmer_15_window_10_byte_8_packed_22_groups
                    == kmer_15_window_10_byte_8_uint32_hash_candidate_blocks
                && kmer_15_window_10_byte_8_packed_22_lane_visits
                    == kmer_15_window_10_byte_8_uint32_hash_lane_visits
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0);
        const std::uint64_t route_calls = generic_selected
            ? generic_u64_kernel_calls
            : kmer_15_window_10_byte_8_uint32_hash_kernel_calls;
        const bool exact_backend = !saturated
            && fail_closed_rejections == 0
            && bridge_symbols_staged == 0
            && bridge_vector_runs == 0
            && scalar_backend_branches == 0
            && scalar_tail_symbols == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_event_records == 0
            && scalar_sort_records == 0
            && scalar_dedup_records == 0
            && packed_decode_lanes == bp_consumed
            && fragment_symbols_staged == bp_consumed
            && ((bp_consumed == 0 && packed_decode_batches == 0)
                || (bp_consumed > 0 && packed_decode_batches > 0))
            && typed_route_complete;
        if (!exact_backend) return false;

        const bool has_vector_activity = vector_batches != 0
            || vector_lanes_visited != 0
            || generic_u64_candidate_blocks != 0
            || kmer_15_window_10_byte_8_uint32_hash_candidate_blocks != 0
            || kmer_15_window_10_byte_8_packed_22_groups != 0
            || generic_u64_lane_visits != 0
            || kmer_15_window_10_byte_8_uint32_hash_lane_visits != 0
            || kmer_15_window_10_byte_8_packed_22_lane_visits != 0;
        if (!has_vector_activity) {
            const bool flush_batches_complete =
                event_generation_batches == route_calls
                && stable_compress_batches == route_calls;
            const bool empty_stage_consistent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && flush_batches_complete
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0;
            return empty_stage_consistent
                && !require_nonempty_vector_work;
        }

        const bool event_routes_complete =
            direct_event_compress_batches <= maximum - tie_matrix_event_batches
            && direct_event_compress_batches + tie_matrix_event_batches
                == vector_batches
            && (tie_matrix_event_batches == 0
                || (row_transpose_batches > 0 && row_vector_stores > 0));
        const bool event_batches_complete =
            vector_batches <= maximum - route_calls
            && event_generation_batches == vector_batches + route_calls;
        const bool stable_batches_complete =
            vector_batches <= maximum - route_calls
            && stable_compress_batches >= vector_batches + route_calls;
        const bool active_stage_consistent =
            vector_batches > 0
            && vector_lanes_visited > 0
            && consecutive_kmer_batches == canonical_scramble_batches
            && consecutive_kmer_batches <= vector_batches
            && window_reduction_batches == vector_batches
            && event_batches_complete
            && stable_batches_complete
            && event_routes_complete
            && packed_decode_batches > 0
            && packed_decode_lanes > 0
            && typed_route_complete;
        if (!active_stage_consistent) return false;
        return !require_nonempty_vector_work
            || (bp_consumed > 0
                && consecutive_kmer_batches > 0
                && canonical_scramble_batches > 0);
    }

    /** @brief Compares every recorded counter and status flag. */
    [[nodiscard]] constexpr bool operator==(
        const two_stack_execution_stats&) const noexcept = default;
};

/** Final status and counters from a two-stack continuation. */
struct two_stack_finalize_result final {
    two_stack_continuation_status status = two_stack_continuation_status::ok;
    two_stack_execution_stats stats{};

    /** @brief Reports whether continuation finalization succeeded. */
    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == two_stack_continuation_status::ok;
    }
};

/** @brief Status of one bounded packed ntHash execution. */
enum class bounded_nthash_status : std::uint8_t {
    ok = 0,
    invalid_configuration,
    input_too_short,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    invalid_range,
    input_too_large,
};

/** Stable persisted identity for the bounded ntHash scheme. */
inline constexpr std::uint32_t k_bounded_nthash_scheme_id =
    0x504E5431u;
inline constexpr std::size_t k_bounded_nthash_max_window = 64;

/**
 * @brief Counts bounded ntHash work and records its input and SIMD settings.
 *
 * SIMD selects the hashes and window minima. Scalar collector appends only
 * transfer selected records. The consistency check rejects scalar hashing,
 * scalar window selection and fallback calls.
 */
struct bounded_nthash_execution_stats {
    std::uint32_t scheme_id = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t nthash_rotation = 0;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t segments_combined = 0;
    std::uint64_t logical_streams = 0;
    std::uint64_t physical_vector_lanes = 0;
    std::uint64_t vector_registers_per_step = 0;
    std::uint64_t packed_decode_batches = 0;
    /** SIMD lane visits, including symbols revisited in overlapping k-mer and window context. */
    std::uint64_t packed_decode_lane_visits = 0;
    std::uint64_t hash_initialization_batches = 0;
    std::uint64_t rolling_hash_batches = 0;
    std::uint64_t hash_table_lookup_batches = 0;
    std::uint64_t canonical_sum_batches = 0;
    std::uint64_t window_update_batches = 0;
    std::uint64_t suffix_rebuild_batches = 0;
    std::uint64_t emission_decision_batches = 0;
    std::uint64_t emission_mask_store_batches = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t tied_endpoint_records_emitted = 0;
    std::uint64_t stream_boundary_records_buffered = 0;
    std::uint64_t stream_boundary_records_committed = 0;
    std::uint64_t stream_boundary_duplicates_suppressed = 0;
    std::uint64_t scalar_collector_appends = 0;
    std::uint64_t partial_chunk_callback_batches = 0;
    std::uint64_t masked_tail_lane_visits = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_minimum_decisions = 0;
    std::uint64_t fallback_calls = 0;
    bool positions_are_segment_local = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    /** @brief Compares every identity, counter, and status field. */
    [[nodiscard]] constexpr bool operator==(
        const bounded_nthash_execution_stats&) const noexcept = default;

    /**
     * @brief Adds a counter, stopping at UINT64_MAX on overflow.
     * did_saturate remains set after an overflow.
     */
    static void saturating_add(
        std::uint64_t& destination,
        std::uint64_t increment,
        bool& did_saturate) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (increment > maximum - destination) {
            destination = maximum;
            did_saturate = true;
        } else {
            destination += increment;
        }
    }

    /**
     * @brief Adds another segment's counters and flags incompatible settings.
     * An empty aggregate takes the first segment's settings.
     */
    void merge(const bounded_nthash_execution_stats& other) noexcept {
        if (other == bounded_nthash_execution_stats{}) return;
        if (*this == bounded_nthash_execution_stats{}) {
            *this = other;
            return;
        }
        const bool same_identity =
            segments_combined > 0
            && other.segments_combined > 0
            && scheme_id == other.scheme_id
            && kmer_size == other.kmer_size
            && window_size == other.window_size
            && nthash_rotation == other.nthash_rotation
            && source_storage_unit_bytes
                == other.source_storage_unit_bytes
            && logical_streams == other.logical_streams
            && physical_vector_lanes == other.physical_vector_lanes
            && vector_registers_per_step
                == other.vector_registers_per_step
            && positions_are_segment_local
                == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
#define BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(name) \
        saturating_add(name, other.name, saturated)
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(input_symbols);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(input_storage_units);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(segments_combined);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(packed_decode_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(packed_decode_lane_visits);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(hash_initialization_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(rolling_hash_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(hash_table_lookup_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(canonical_sum_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(window_update_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(suffix_rebuild_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(emission_decision_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(emission_mask_store_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(minimizers_emitted);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(tied_endpoint_records_emitted);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_records_buffered);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_records_committed);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_duplicates_suppressed);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_collector_appends);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(partial_chunk_callback_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(masked_tail_lane_visits);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(fail_closed_rejections);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_hash_symbols);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_window_transitions);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_minimum_decisions);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(fallback_calls);
#undef BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER
    }

    /**
     * @brief Checks ntHash settings and relationships between its work counters.
     * Rejects mixed settings, overflow and scalar fallback work.
     * require_nonempty_vector_work also requires nonzero SIMD stage counts.
     */
    [[nodiscard]] constexpr bool is_internally_consistent(
        bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok = !require_nonempty_vector_work
            || (packed_decode_batches > 0
                && packed_decode_lane_visits > 0
                && hash_initialization_batches > 0
                && hash_table_lookup_batches > 0
                && canonical_sum_batches > 0
                && window_update_batches > 0
                && emission_decision_batches > 0
                && emission_mask_store_batches > 0);
        return !saturated
            && !aggregation_mismatch
            && scheme_id == k_bounded_nthash_scheme_id
            && kmer_size > 0
            && window_size > 0
            && window_size <= k_bounded_nthash_max_window
            && nthash_rotation == 7
            && source_storage_unit_bytes >= sizeof(std::uint32_t)
            && logical_streams == 8
            && (physical_vector_lanes == 4
                || physical_vector_lanes == 8)
            && vector_registers_per_step * physical_vector_lanes == 8
            && segments_combined > 0
            && positions_are_segment_local
            && vector_work_ok
            && canonical_sum_batches == window_update_batches
            && minimizers_emitted == scalar_collector_appends
            && stream_boundary_records_buffered
                == stream_boundary_records_committed
                    + stream_boundary_duplicates_suppressed
            && fail_closed_rejections == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_minimum_decisions == 0
            && fallback_calls == 0;
    }
};

/** @brief Bounded ntHash status and work counters. */
struct bounded_nthash_result {
    bounded_nthash_status status = bounded_nthash_status::ok;
    bounded_nthash_execution_stats stats{};

    /** @brief Reports whether the bounded ntHash kernel succeeded. */
    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == bounded_nthash_status::ok;
    }

    /** @brief Requires a successful result with consistent ntHash work counters. */
    [[nodiscard]] constexpr bool has_valid_kernel_stats(
        bool require_nonempty_vector_work = true) const noexcept {
        return is_ok() && stats.is_internally_consistent(require_nonempty_vector_work);
    }
};

} // namespace bioseq::minimizers
