/**
 * @file state.cppm
 * @brief Exact two-stack target-local descriptors and continuation state.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_state;

import std;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::minimizers::minimap_sketch_data;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::minimizer_kernel_algorithm;
using ::bioseq::minimizers::minimizer_kernel_invocation;
using ::bioseq::minimizers::minimizer_kernel_result;
using ::bioseq::minimizers::minimizer_kernel_status;
using ::bioseq::minimizers::minimizer_output_layout;
using ::bioseq::minimizers::minimizer_output_position_mask;
using ::bioseq::minimizers::two_stack_route;

/**
 * @brief Compile-time upper bound on the supported two-stack window size.
 *
 * The minimap2 ports run at the
 * default `w = 10`. Bumping past 256 also blows the L1-friendly stack
 * footprint of the parallel ring buffers.
 */
using ::bioseq::minimizers::k_max_two_stack_window;

/** Maximum nt4 k-mer size represented by one native Highway integer lane. */
using ::bioseq::minimizers::k_max_two_stack_kmer;

/** Largest k-mer accepted by the exact scalar-oracle arithmetic. */
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;

using ::bioseq::minimizers::two_stack_continuation_status;
using ::bioseq::minimizers::exact_simd_kernel;
using ::bioseq::minimizers::two_stack_continuation_status_name;
using ::bioseq::minimizers::two_stack_execution_stats;
using ::bioseq::minimizers::two_stack_finalize_result;
using ::bioseq::minimizers::bounded_nthash_status;
using ::bioseq::minimizers::k_bounded_nthash_scheme_id;
using ::bioseq::minimizers::k_bounded_nthash_max_window;
using ::bioseq::minimizers::bounded_nthash_execution_stats;
using ::bioseq::minimizers::bounded_nthash_result;
using ::bioseq::minimizers::exact_simd_runtime_error;
using ::bioseq::minimizers::exact_simd_minimizer_representation;

namespace resident_descriptor_detail {

/// @brief Non-owning header for a resident packed sequence descriptor.
struct borrowed_packed_header {
    const void* data = nullptr;
    bioseq::storage::sequence_size_t bp_size = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
};

/// @brief Adapts a borrowed packed header to the compact-sequence protocol.
/// @tparam p_alphabet Logical alphabet stored by the descriptor.
/// @tparam p_storage_unit Packed storage unit exposed by the range.
template <sequence_alphabet p_alphabet, class p_storage_unit>
struct borrowed_packed_instance {
    /** @brief Logical alphabet stored by the borrowed sequence. */
    using alphabet_type = p_alphabet;
    /** @brief Packed unit used to interpret the borrowed storage. */
    using storage_unit = p_storage_unit;
    /** @brief Header type retained by the compact range. */
    using compact_sequence_stdc_type = borrowed_packed_header;

    /** @brief Logical bits stored for each alphabet symbol. */
    static constexpr std::size_t letter_bit_width =
        bioseq::storage::lazy_sequence_view<p_alphabet>::traits::
            k_logical_bits_per_symbol;
    /** @brief Width of one packed storage unit in bytes. */
    static constexpr std::size_t byte_width = sizeof(p_storage_unit);
    /** @brief Marks this adapter as a compact-sequence instance. */
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/// @brief Invokes a function with a typed non-owning compact range.
/// @tparam p_alphabet Logical alphabet stored by the descriptor.
/// @tparam p_storage_unit Packed storage unit selected by dispatch.
/// @tparam p_function Callable accepting the temporary compact range.
/// @param descriptor Borrowed packed storage and logical geometry.
/// @param fn Callable invoked synchronously while the temporary header is alive.
/// @return The callable's result, including reference category.
template <sequence_alphabet p_alphabet, class p_storage_unit, class p_function>
decltype(auto) with_borrowed_compact_range(
        const typename bioseq::storage::lazy_sequence_view<p_alphabet>::
            resident_packed_sequence& descriptor,
        p_function&& fn) {
    using instance = borrowed_packed_instance<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<instance>;
    borrowed_packed_header header{
        .data = descriptor.units,
        .bp_size = descriptor.logical_length,
        .storage_unit_byte_width = descriptor.storage_unit_width,
        .padding_letters = descriptor.trailing_padding,
    };
    range_type range{header};
    return std::forward<p_function>(fn)(range);
}

}  // namespace resident_descriptor_detail

/**
 * @brief Retains fragment continuation for the nt4 two-stack kernel.
 *
 * Pending capacity follows the generated target's Highway lane count.
 * @tparam p_window_size Compile-time window size, or zero for runtime selection.
 * @tparam p_kmer_size Compile-time k-mer size, or zero for runtime selection.
 * @tparam p_alphabet_traits Packed-sequence alphabet and storage traits.
 */
template<std::size_t p_window_size, std::size_t p_kmer_size, class p_alphabet_traits>
struct two_stack_continuation_state {
    /** @brief Maximum u64 lanes supported by the compiled Highway targets. */
    static constexpr std::size_t max_vector_lanes =
        hn::MaxLanes(hn::ScalableTag<std::uint64_t>{});
    /** @brief Candidate count processed by the fixed 15/10 fast block. */
    static constexpr std::size_t fast_block_candidates = 8;
    /** @brief Storage available for decoded candidates awaiting SIMD work. */
    static constexpr std::size_t pending_capacity =
        std::max(max_vector_lanes, fast_block_candidates);

    /** @brief Packed-sequence traits carried by this state. */
    using traits = p_alphabet_traits;
    /** @brief Logical alphabet described by @ref traits. */
    using alphabet_type = typename traits::alphabet_type;

    // Static routes retain their own bounded window and k-mer capacities.
    // scratch footprint.  This state is instantiated inside the per-sketch
    // driver, and its arrays participate in the hot candidate-block frame.
    /** @brief Capacity for symbols preceding the next fragment boundary. */
    static constexpr std::size_t code_tail_capacity =
        p_kmer_size != 0 && p_kmer_size <= k_max_exact_two_stack_kmer
            ? std::max<std::size_t>(p_kmer_size - 1, 1)
            : k_max_exact_two_stack_kmer - 1;
    /** @brief Capacity for candidates preceding the next window boundary. */
    static constexpr std::size_t candidate_tail_capacity =
        p_window_size != 0 && p_window_size <= k_max_two_stack_window
            ? std::max<std::size_t>(p_window_size - 1, 1)
            : k_max_two_stack_window - 1;

    /// @brief Stores one chronological minimizer candidate.
    struct candidate_state {
        std::uint64_t hash = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t raw = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t ordinal = std::numeric_limits<std::uint64_t>::max();
    };

    std::size_t runtime_w = 0;
    std::size_t runtime_k = 0;
    std::size_t emitted_position_base = 0;
    std::size_t input_position_base = 0;
    std::size_t next_input_position = 0;
    std::uint64_t valid_run_length = 0;
    std::array<std::uint8_t, code_tail_capacity> code_tail{};
    std::size_t code_tail_size = 0;
    std::array<std::uint8_t, pending_capacity> pending_codes{};
    std::array<std::uint64_t, pending_capacity> pending_positions{};
    std::array<std::uint64_t, pending_capacity> pending_valid_run_lengths{};
    std::size_t pending_count = 0;
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_hash{};
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_raw{};
    std::array<std::uint64_t, candidate_tail_capacity> candidate_tail_ordinal{};
    candidate_state current_min{};
    std::uint64_t candidate_count = 0;
    bool candidate_state_initialized = false;
    bool initialized = false;
    bool saw_first_fragment = false;
    bool finalized = false;
    exact_simd_kernel kernel_kind = exact_simd_kernel::generic_u64;
    two_stack_continuation_status status = two_stack_continuation_status::ok;
    two_stack_execution_stats stats{};

    /// @brief Returns the compile-time or runtime window size.
    [[nodiscard]] constexpr std::size_t window_size() const noexcept {
        if constexpr (p_window_size != 0) return p_window_size;
        return runtime_w;
    }

    /// @brief Returns the compile-time or runtime k-mer size.
    [[nodiscard]] constexpr std::size_t kmer_size() const noexcept {
        if constexpr (p_kmer_size != 0) return p_kmer_size;
        return runtime_k;
    }

};

}  // namespace target
}  // namespace bioseq::simd
