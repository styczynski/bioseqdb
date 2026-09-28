/**
 * @file state.cppm
 * @brief Packed input views and sxate kept between minimizer fragments.
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
 * @brief Maximum minimizer window size, 256 k-mers.
 *
 * This bounds the candidate buffers kept on the stack. The  usual minimap2
 * window contains 10 k-mers.
 */
using ::bioseq::minimizers::k_max_two_stack_window;

/** Maximum DNA4 k-mer, length that fits in a 64-bit lane: 32 bases. */
using ::bioseq::minimizers::k_max_two_stack_kmer;

/** Largest k-mer length accepted by the exact minimizer calculation: 31 bases. */
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
using ::bioseq::minimizers::wide_simd_minimizer_representation;
using ::bioseq::minimizers::two_stack_simd_minimizer_representation;

namespace resident_descriptor_detail {

/// @brief Pointer, length, and packing details for a borrowed sequence.
struct borrowed_packed_header {
    const void* data = nullptr;
    bioseq::storage::sequence_size_t bp_size = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
};

/**
 * @brief Lets compact_sequence_range read borrowed packed storage.
 * @tparam p_alphabet Alphabet encoded in the storage.
 * @tparam p_storage_unit Integer type used for each packed unit.
 */
template <sequence_alphabet p_alphabet, class p_storage_unit>
struct borrowed_packed_instance {
    /** @brief Logical alphabet stored by the borrowed sequence. */
    using alphabet_type = p_alphabet;
    /** @brief Packed unit used to interpret the borrowed storage. */
    using storage_unit = p_storage_unit;
    /** @brief Header type borrowed by the compact range. */
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

/**
 * @brief Calls fn with a compact range over borrowed packed storage.
 *
 * The temporary header and range live only during this call. fn must not
 * keep references to them after it returns. The  underlying storage remains owned by the
 * caller
 * @return The result of fn, preserving references.
 */
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
 * @brief Keeps k-mer and window state across sequence fragments.
 *
 * The symbol tail joins k-mers across fragment boundaries. Pending symbols
 * wait for a SIMD batch, while the candidate tail and current minimum keep
 * the minimizer window consinuous.
 * @tparam p_window_size Fixed window size, or zero to select it at runtime.
 * @tparam p_kmer_size Fixed k-mer size, or zero to select it at runtime.
 * @tparam p_alphabet_traits Alphabet and packed storage types.
 */
template<std::size_t p_window_size, std::size_t p_kmer_size, class p_alphabet_traits>
struct two_stack_continuation_state {
    /** @brief Maximum u64 lanes for this compiled Highway target. */
    static constexpr std::size_t max_vector_lanes =
        hn::MaxLanes(hn::ScalableTag<std::uint64_t>{});
    /** @brief Candidates processed together  by the K15/W10 fast kernel. */
    static constexpr std::size_t fast_block_candidates = 8;
    /** @brief Space for symbols waiting for the next SIMD batch. */
    static constexpr std::size_t pending_capacity =
        std::max(max_vector_lanes, fast_block_candidates);

    /** @brief Packed-sequence traits carried by this state. */
    using traits = p_alphabet_traits;
    /** @brief Logical alphabet described by @ref traits. */
    using alphabet_type = typename traits::alphabet_type;

    // Fixed k-mer and window sizes keep the per-sketch scratch arrays small.
    /** @brief Space for the k-1 symbols needed to continue the next k-mer. */
    static constexpr std::size_t code_tail_capacity =
        p_kmer_size != 0 && p_kmer_size <= k_max_exact_two_stack_kmer
            ? std::max<std::size_t>(p_kmer_size - 1, 1)
            : k_max_exact_two_stack_kmer - 1;
    /** @brief Space for the w-1 candidates needed to continue the next window. */
    static constexpr std::size_t candidate_tail_capacity =
        p_window_size != 0 && p_window_size <= k_max_two_stack_window
            ? std::max<std::size_t>(p_window_size - 1, 1)
            : k_max_two_stack_window - 1;

    /// @brief Hash, encoded output, and input order of one minimizer candidate.
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
