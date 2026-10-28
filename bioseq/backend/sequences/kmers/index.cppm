/**
 * @file index.cppm
 * @brief Collects minimizers from compact and lazy sequence views.
 */
module;

#include <libassert/assert.hpp>
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

export module bioseq.sequences.kmers;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.bounded_nthash_seed_policy;
import bioseq.common.circular_append_buffer;
import bioseq.instrumentation;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::simd {

/** Export shared minimizer records and collectors throueh, this  module. */
// clang-format off
using ::bioseq::minimizers::minimizer_layout;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimap_sketch_data;
static_assert(std::same_as<minimap_sketch_data, minimizer_representation<28u, 32u>>);

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
          class p_collector_type = basic_sketch_collector<p_minimizer_type>,
          ::bioseq::minimizers::minimizer_kernel_algorithm p_hash_algorithm =
              ::bioseq::minimizers::minimizer_kernel_algorithm::minimap2_reference>
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
    static constexpr bool k_native_rolling = p_hash_algorithm ==
        ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash;
    static_assert(k_native_rolling || p_hash_algorithm ==
        ::bioseq::minimizers::minimizer_kernel_algorithm::minimap2_reference);
    static constexpr bool k_is_nucleotide = !k_native_rolling
        && seqan3::nucleotide_alphabet<alphabet_type>;
    using hash_alphabet = std::conditional_t<k_is_nucleotide, seq::dna4, alphabet_type>;
    using hasher_type = std::conditional_t<k_native_rolling,
        typename ::bioseq::minimizers::bounded_nthash_seed_policy<alphabet_type>::scalar_state,
        typename p_scalar_hasher_template<hash_alphabet>::template scalar_hash<p_hash_value>>;

    // The power-of-two capacity keeps circular-buffer wraparound mask-based.
    constexpr std::uint64_t k_max_window_span = 256u;
    DEBUG_ASSERT(kmer_size != 0u && kmer_size <= hasher_type::traits::k_max_kmer_length,
        "exact minimizer k-mer exceeds the selected native-rank word capacity");
    DEBUG_ASSERT(window_size != 0u && window_size <= (k_native_rolling
        ? ::bioseq::minimizers::k_bounded_nthash_max_window : k_max_window_span),
        "scalar minimizer window exceeds the selected scheme capacity");
    hasher_type hasher(kmer_size);

    // The native scheme retains both minimum endpoints, sharing the packed
    // traversal and record ring with exact scalar selection below.
    using endpoint_pair = std::pair<p_minimizer_type, p_minimizer_type>;
    using optional_endpoints = std::optional<endpoint_pair>;
    std::array<std::uint32_t, k_native_rolling ? hasher_type::traits::k_max_kmer_length : 0u> native_rank_ring{};
    std::array<optional_endpoints, k_native_rolling
        ? ::bioseq::minimizers::k_bounded_nthash_max_window : 0u> native_suffix{};
    optional_endpoints native_prefix{};
    std::size_t native_rank_slot = 0u;
    std::size_t native_window_slot = 0u;
    std::optional<p_minimizer_type> native_last_emitted{};
    const auto combine_endpoints = [](const optional_endpoints& first,
                                      const optional_endpoints& second) -> optional_endpoints {
        if (!first) return second;
        if (!second) return first;
        if (first->first.kmer() < second->first.kmer()) return first;
        if (second->first.kmer() < first->first.kmer()) return second;
        return endpoint_pair{
            first->first.last_pos() <= second->first.last_pos() ? first->first : second->first,
            first->second.last_pos() >= second->second.last_pos() ? first->second : second->second};
    };

    bioseq::common::circular_append_buffer<p_minimizer_type, k_max_window_span> buffer(window_size);

    std::array<bool, k_max_window_span> buffer_valid{};
    p_minimizer_type current_min{};
    bool current_min_valid = false;
    auto current_min_pos = buffer.get_begin_position();

    std::size_t valid_length = 0u;
    std::size_t sequence_position = 0u;
    auto iter = sequence.template iter<logical_rank>(sequence_begin);
    const auto end = sequence.template iter<logical_rank>(sequence_end);

    // Fill the first  window, then continue using the same rolling state.
    auto collect_range = [&collector,
                          &iter,
                          &end,
                          &valid_length,
                          &window_size,
                          &kmer_size,
                          &sequence_position,
                          &buffer,
                          &buffer_valid,
                          &hasher,
                          &current_min_valid,
                          &current_min,
                          &current_min_pos, &native_rank_ring, &native_suffix,
                          &native_prefix, &native_rank_slot, &native_window_slot,
                          &native_last_emitted, &combine_endpoints]<bool p_is_initializing>(
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
                is_ambiguous = code >= seqan3::alphabet_size<alphabet_type>;
            }
            if (is_ambiguous) {
                // Mark this position unusable and restart k-mer construction.
                buffer.push_back(p_minimizer_type{});
                buffer_valid[buffer.get_last_write_position()] = false;
                valid_length = 0u;
                hasher.reset();
                if constexpr (k_native_rolling) {
                    native_rank_slot = 0u;
                    native_window_slot = 0u;
                    native_prefix.reset();
                    for (auto& endpoints : native_suffix) endpoints.reset();
                }
            } else {
                const auto [canonical_hash, strand] = [code, &hasher, &valid_length,
                    &kmer_size, &native_rank_ring, &native_rank_slot]() {
                    if constexpr (k_native_rolling) {
                        if (valid_length < kmer_size) {
                            native_rank_ring[valid_length] = code;
                            hasher.append_initial(code, valid_length);
                        } else {
                            hasher.update(code, native_rank_ring[native_rank_slot]);
                            native_rank_ring[native_rank_slot] = code;
                            if (++native_rank_slot == kmer_size) native_rank_slot = 0u;
                        }
                        return hasher.value();
                    } else {
                        return hasher.next(static_cast<p_hash_value>(code));
                    }
                }();

                // An excluded palindrome occupies an explicitly invalid window slot.
                ++valid_length;

                if (strand == sequence_strand_mode::equal) {
                    buffer.push_back(p_minimizer_type{});
                    buffer_valid[buffer.get_last_write_position()] = false;
                } else if (valid_length >= kmer_size) {
                    buffer.push_back(p_minimizer_type(
                        /*kmer=*/canonical_hash,
                        /*last_pos=*/sequence_position,
                        /*strand=*/static_cast<std::uint64_t>(strand)));
                    buffer_valid[buffer.get_last_write_position()] = true;
                } else {
                    buffer.push_back(p_minimizer_type{});
                    buffer_valid[buffer.get_last_write_position()] = false;
                }
            }

            if constexpr (k_native_rolling) {
                if (!buffer_valid[buffer.get_last_write_position()]) continue;
                const auto& entering_record = buffer.get_last_value();
                const optional_endpoints entering{endpoint_pair{entering_record, entering_record}};
                const std::size_t written_slot = native_window_slot;
                if (++native_window_slot == window_size) native_window_slot = 0u;
                native_suffix[written_slot] = entering;
                native_prefix = combine_endpoints(native_prefix, entering);
                if (native_window_slot == 0u) {
                    for (std::size_t slot = window_size - 1u; slot > 0u; --slot) {
                        native_suffix[slot - 1u] = combine_endpoints(
                            native_suffix[slot - 1u], native_suffix[slot]);
                    }
                    native_prefix = entering;
                }
                const auto minimum = combine_endpoints(native_prefix, native_suffix[native_window_slot]);
                if (valid_length >= kmer_size + window_size - 1u && minimum) {
                    const auto emit = [&collector, &native_last_emitted](const p_minimizer_type& record) {
                        if (!native_last_emitted || native_last_emitted->kmer() != record.kmer()
                            || native_last_emitted->pos_strand_encoded() != record.pos_strand_encoded()) {
                            collector.collect(record);
                            native_last_emitted = record;
                        }
                    };
                    emit(minimum->first);
                    if (minimum->second.last_pos() != minimum->first.last_pos()) emit(minimum->second);
                }
                continue;
            }

            if ((valid_length == window_size + kmer_size - 1 && p_is_initializing) &&
                current_min_valid) { // first full window
                // Emit the other positions tied with the first window's minimum.
                buffer.for_each_filled_value_unsafe(
                    [&collector, &current_min, &buffer_valid](const auto& index, const auto& candidate, const auto&) {
                        if (buffer_valid[index] && p_minimizer_type::does_sketch_match(current_min, candidate)) {
                            collector.collect(candidate);
                        }
                    });
            }
            if (buffer_valid[buffer.get_last_write_position()]
                && (!current_min_valid || buffer.get_last_value() <= current_min)) {
                if ((valid_length >= window_size + kmer_size || !p_is_initializing) &&
                    current_min_valid) {
                    collector.collect(current_min);
                }
                current_min_pos = buffer.get_last_write_position();
                current_min = buffer.get_last_value();
                current_min_valid = true;
            } else if (buffer.get_last_write_position() == current_min_pos) {
                if ((valid_length >= window_size + kmer_size - 1 || !p_is_initializing) &&
                    current_min_valid) {
                    collector.collect(current_min);
                }

                current_min_valid = false;
                // Recomputes the rightmost minimum after its ring slot expires.
                buffer.for_each_filled_value_unsafe(
                    [&current_min, &current_min_valid, &buffer_valid,
                     &current_min_pos](const auto& index, const auto& candidate, const auto&) {
                        if (buffer_valid[index] && (!current_min_valid || current_min >= candidate)) {
                            current_min = candidate;
                            current_min_valid = true;
                            current_min_pos = index;
                        }
                    });

                if ((valid_length >= window_size + kmer_size - 1 || !p_is_initializing) &&
                    current_min_valid) { // emit other tied positions
                    // Emit tied records with a different position or strand.
                    buffer.for_each_filled_value_unsafe(
                        [&collector, &buffer_valid,
                         &current_min](const auto& index, const auto& candidate, const auto&) {
                            if (buffer_valid[index] && p_minimizer_type::does_sketch_match(current_min, candidate)) {
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

    if constexpr (!k_native_rolling) {
        if (current_min_valid) collector.collect(current_min);
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
 * @throws std::out_of_range if the interval lies outside the sequence  or is reversed.
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
