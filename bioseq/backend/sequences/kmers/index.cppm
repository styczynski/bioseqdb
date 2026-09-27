/**
 * @file index.cppm
 * @brief Collects minimizers from compact and lazy sequence views.
 */
module;

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

export module bioseq.sequences.kmers;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers.hashers;
import bioseq.common.circular_append_buffer;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::simd {

/** Export shared minimizer records and collectors through this module. */
// clang-format off
using ::bioseq::minimizers::minimizer_layout;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimap_sketch_data;
static_assert(std::same_as<minimap_sketch_data, minimizer_representation<28u, 15u>>);

using ::bioseq::minimizers::k_max_minimizer_sequence_length;
using ::bioseq::minimizers::is_minimizer_query_length_representable;
using ::bioseq::minimizers::require_representable_minimizer_query_length;
using ::bioseq::minimizers::basic_sketch_collector;
using ::bioseq::minimizers::sketch_collector;
using ::bioseq::minimizers::admitted_sketch_collector;
// clang-format on

/** @brief Number of bits needed for one logical alphabet symbol. */
template <class p_sequence_view>
inline constexpr std::size_t logical_bits_per_symbol_v = [] {
    using traits = typename std::remove_cvref_t<p_sequence_view>::traits;
    if constexpr (requires { traits::k_logical_bits_per_symbol; }) {
        return static_cast<std::size_t>(traits::k_logical_bits_per_symbol);
    } else {
        return static_cast<std::size_t>(traits::bits_per_char);
    }
}();

/**
 * @brief Collects scalar minimizers from the selected sequence interval.
 *
 * Rolling hashes feed a circular window. A new minimum replaces the current
 * one, and an expired minimum triggers a window scan. Tied hashes keep
 * their separate positions. Ambiguous nucleotides restart k-mer construction.
 * Reported positions start at zero within the selected interval.
 */
template <template <typename p_hash_alphabet> typename p_scalar_hasher_template,
          class p_minimizer_type,
          class p_sequence_view,
          typename p_iterator = decltype(std::declval<p_sequence_view&>().begin()),
          typename p_hash_value = std::uint64_t,
          class p_collector_type = basic_sketch_collector<p_minimizer_type>>
    requires(supports_scalar_hash<p_scalar_hasher_template,
                                  typename p_sequence_view::traits::alphabet_type,
                                  p_hash_value>)
[[gnu::always_inline]]
inline void collect_minimizers_view(p_collector_type& collector,
                                    const p_sequence_view& sequence,
                                    const p_iterator& sequence_begin,
                                    const p_iterator& sequence_end,
                                    const std::size_t window_size,
                                    const std::size_t kmer_size) {
    static const auto* region_description =
        bioseq::instrumentation::register_static_region("mm_sketch");
    auto region_scope = bioseq::instrumentation::region(region_description);

    using sequence_traits = typename p_sequence_view::traits;
    using alphabet_type = typename sequence_traits::alphabet_type;
    // Convert nucleotide ranks to minimap2's A/C/G/T codes. DNA5 N
    // becomes the ambiguity sentinel. Other alphabets keep their ranks.
    static constexpr bool k_is_nucleotide = seqan3::nucleotide_alphabet<alphabet_type>;
    using hash_alphabet = std::conditional_t<k_is_nucleotide, seq::dna4, alphabet_type>;
    using hasher_type =
        typename p_scalar_hasher_template<hash_alphabet>::template scalar_hash<p_hash_value>;

    // The power-of-two capacity keeps circular-buffer wraparound mask-based.
    constexpr std::uint64_t k_max_window_span = 256u;
    hasher_type hasher(kmer_size);

    bioseq::common::circular_append_buffer<p_minimizer_type, k_max_window_span> buffer(window_size);

    p_minimizer_type current_min = p_minimizer_type::maximum();
    auto current_min_pos = buffer.get_begin_position();

    std::size_t valid_length = 0u;
    std::size_t sequence_position = 0u;
    auto iter = sequence.template iter<logical_rank>(sequence_begin);
    const auto end = sequence.template iter<logical_rank>(sequence_end);

    // Fill the first window, then continue using the same rolling state.
    auto collect_range = [&collector,
                          &iter,
                          &end,
                          &valid_length,
                          &window_size,
                          &kmer_size,
                          &sequence_position,
                          &buffer,
                          &hasher,
                          &current_min,
                          &current_min_pos]<bool p_is_initializing>(
                             std::bool_constant<p_is_initializing>) {
        for (; iter != end && (valid_length < window_size + kmer_size || !p_is_initializing);
             ++iter, ++sequence_position) {
            const auto nucl = *iter;
            // An nt4 sentinel restarts the run of valid nucleotide symbols.
            std::uint8_t code;
            bool is_ambiguous;
            if constexpr (k_is_nucleotide) {
                code = nt4_normalize<alphabet_type>(static_cast<std::uint8_t>(nucl));
                is_ambiguous = (code >= k_nt4_sentinel);
            } else {
                code = static_cast<std::uint8_t>(nucl);
                is_ambiguous = (logical_bits_per_symbol_v<p_sequence_view> > 2 && nucl >= 4);
            }
            if (is_ambiguous) {
                // Mark this position unusable and restart k-mer construction.
                buffer.push_back(p_minimizer_type::maximum());
                valid_length = 0u;
                // Reconstruct the hasher in place because it cannot be copy-assigned.
                hasher.~hasher_type();
                ::new (static_cast<void*>(&hasher)) hasher_type(kmer_size);
            } else {
                const auto [canonical_hash, strand] =
                    hasher.next(static_cast<p_hash_value>(code), static_cast<p_hash_value>(code));

                // A palindromic k-mer occupies a window slot with the maximum sentinel.
                // The SIMD path uses the same rule, so neither emits a palindrome.
                // Minimap2 itself skips that slot, which can change later window minima.
                const auto kmer_span = (valid_length + 1 < kmer_size && p_is_initializing)
                                           ? valid_length + 1
                                           : kmer_size;
                ++valid_length;

                if (strand == sequence_strand_mode::equal) {
                    buffer.push_back(p_minimizer_type::maximum());
                } else if (valid_length >= kmer_size) {
                    // Compact records carry no span field. The constructor uses its
                    // default span for wider record formats.
                    (void)kmer_span;
                    buffer.push_back(p_minimizer_type(
                        /*kmer=*/canonical_hash,
                        /*last_pos=*/sequence_position,
                        /*strand=*/static_cast<std::uint64_t>(strand)));
                } else {
                    buffer.push_back(p_minimizer_type::maximum());
                }
            }

            if ((valid_length == window_size + kmer_size - 1 && p_is_initializing) &&
                !current_min.is_max()) { // first full window
                // Emit the other positions tied with the first window's minimum.
                buffer.for_each_filled_value_unsafe(
                    [&collector, &current_min](const auto&, const auto& candidate, const auto&) {
                        if (p_minimizer_type::does_sketch_match(current_min, candidate)) {
                            collector.collect(candidate);
                        }
                    });
            }
            if (buffer.get_last_value() <= current_min) {
                if ((valid_length >= window_size + kmer_size || !p_is_initializing) &&
                    !current_min.is_max()) {
                    collector.collect(current_min);
                }
                current_min_pos = buffer.get_last_write_position();
                current_min = buffer.get_last_value();
            } else if (buffer.get_last_write_position() == current_min_pos) {
                if ((valid_length >= window_size + kmer_size - 1 || !p_is_initializing) &&
                    !current_min.is_max()) {
                    collector.collect(current_min);
                }

                current_min.set_max();
                // Recomputes the rightmost minimum after its ring slot expires.
                buffer.for_each_filled_value_unsafe(
                    [&current_min,
                     &current_min_pos](const auto& index, const auto& candidate, const auto&) {
                        if (current_min >= candidate) {
                            current_min = candidate;
                            current_min_pos = index;
                        }
                    });

                if ((valid_length >= window_size + kmer_size - 1 || !p_is_initializing) &&
                    !current_min.is_max()) { // emit other tied positions
                    // Emit tied records with a different position or strand.
                    buffer.for_each_filled_value_unsafe(
                        [&collector,
                         &current_min](const auto&, const auto& candidate, const auto&) {
                            if (p_minimizer_type::does_sketch_match(current_min, candidate)) {
                                collector.collect(candidate);
                            }
                        });
                }
            }
        }
    };

    // Initialize through the first complete window, then scan the remaining symbols.
    collect_range(std::bool_constant<true>{});
    collect_range(std::bool_constant<false>{});

    if (!current_min.is_max()) {
        collector.collect(current_min);
    }
}

/** Export backend choices and the position/hash pair type. */
// clang-format off
using ::bioseq::minimizers::minimizer_sketch_backend;
using ::bioseq::minimizers::minimizer_sketch_backend_name;
using ::bioseq::minimizers::is_vulkan_minimizer_backend;
using ::bioseq::minimizers::minimizer_sorted_pair;
// clang-format on

/**
 * @brief Collects scalar minimizers from the lazy range [lo_bp, hi_bp).
 * Output positions are relative to lo_bp.
 * @throws std::out_of_range if the interval lies outside the sequence or is reversed.
 */
template <class p_minimizer_type, sequence_alphabet p_alphabet, class p_collector_type>
[[gnu::always_inline]] inline void
collect_minimizers_segment_scalar(p_collector_type& collector,
                                  bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                  bioseq::storage::sequence_size_t lo_bp,
                                  bioseq::storage::sequence_size_t hi_bp,
                                  std::size_t window_size,
                                  std::size_t kmer_size) {
    if (lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        throw std::out_of_range("minimizer base-pair segment exceeds lazy sequence");
    }
    auto begin_iter = sequence.begin();
    auto end_iter = sequence.begin();
    begin_iter += static_cast<std::ptrdiff_t>(lo_bp);
    end_iter += static_cast<std::ptrdiff_t>(hi_bp);
    collect_minimizers_view<minimap_int_hasher, p_minimizer_type>(
        collector, sequence, begin_iter, end_iter, window_size, kmer_size);
}

/**
 * @brief Number of alphabet symbols stored in one packed sequence unit.
 * @tparam p_sequence Packed sequence type.
 */
template <class p_sequence>
inline constexpr std::size_t letters_per_unit_v =
    std::remove_cvref_t<p_sequence>::traits::letters_per_unit;

/** @brief Rounds a symbol count up to the number of packed storage words needed. */
template <class p_sequence>
[[nodiscard]] inline bioseq::storage::sequence_size_t
unit_count_for_bp(bioseq::storage::sequence_size_t bp) noexcept {
    constexpr bioseq::storage::sequence_size_t lpu =
        static_cast<bioseq::storage::sequence_size_t>(letters_per_unit_v<p_sequence>);
    return (bp + lpu - 1) / lpu;
}

/**
 * @brief Position field mask expressed as the sequence-size type.
 * @tparam p_minimizer_type Stored minimizer representation.
 */
template <class p_minimizer_type>
inline constexpr bioseq::storage::sequence_size_t minimizer_pos_field_mask_v =
    static_cast<bioseq::storage::sequence_size_t>(p_minimizer_type::k_position_field_mask);

/** @brief Checks that zero-based positions across collected_bp symbols fit the record. */
template <class p_minimizer_type>
[[nodiscard]] inline bool
collected_span_fits_minimizer_pos_field(bioseq::storage::sequence_size_t collected_bp) noexcept {
    return collected_bp <= k_max_minimizer_sequence_length<p_minimizer_type>;
}

/**
 * @brief Collects scalar minimizers between compact-sequence iterators.
 * Uses the selected hasher and runtime window and k-mer sizes.
 * Output positions start at zero within the selected interval.
 */
template <template <typename p_hash_alphabet> typename p_scalar_hasher_template,
          class p_minimizer_type,
          supports_compact_sequence_range p_sequence_range,
          typename p_iterator = decltype(compact_sequence_range<p_sequence_range>().begin()),
          typename p_hash_value = std::uint64_t,
          class p_collector_type = basic_sketch_collector<p_minimizer_type>>
    requires(supports_scalar_hash<
             p_scalar_hasher_template,
             typename compact_sequence_range<p_sequence_range>::traits::alphabet_type,
             p_hash_value>)
[[gnu::always_inline]]
inline void collect_minimizers(p_collector_type& collector,
                               compact_sequence_range<p_sequence_range>& sequence,
                               const p_iterator& sequence_begin,
                               const p_iterator& sequence_end,
                               const std::size_t window_size,
                               const std::size_t kmer_size) {
    collect_minimizers_view<p_scalar_hasher_template,
                            p_minimizer_type,
                            compact_sequence_range<p_sequence_range>,
                            p_iterator,
                            p_hash_value,
                            p_collector_type>(
        collector, sequence, sequence_begin, sequence_end, window_size, kmer_size);
}

/** @brief Collects scalar minimizers using template-specified window and k-mer sizes. */
template <std::size_t p_window_size,
          std::size_t p_kmer_size,
          template <typename p_hash_alphabet> typename p_scalar_hasher_template,
          class p_minimizer_type,
          supports_compact_sequence_range p_sequence_range,
          typename p_iterator = decltype(compact_sequence_range<p_sequence_range>().begin()),
          typename p_hash_value = std::uint64_t,
          class p_collector_type = basic_sketch_collector<p_minimizer_type>>
[[gnu::always_inline]]
inline void collect_minimizers_constexpr(p_collector_type& collector,
                                         compact_sequence_range<p_sequence_range>& sequence,
                                         const p_iterator& sequence_begin,
                                         const p_iterator& sequence_end) {
    collect_minimizers<p_scalar_hasher_template,
                       p_minimizer_type,
                       p_sequence_range,
                       p_iterator,
                       p_hash_value,
                       p_collector_type>(
        collector, sequence, sequence_begin, sequence_end, p_window_size, p_kmer_size);
}

} // namespace bioseq::simd
