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

/** Re-export the target-independent minimizer contract through the facade. */
using ::bioseq::minimizers::minimizer_layout;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimap_sketch_data;
static_assert(std::same_as<
    minimap_sketch_data,
    minimizer_representation<28u, 15u>>);

using ::bioseq::minimizers::k_max_minimizer_sequence_length;
using ::bioseq::minimizers::is_minimizer_query_length_representable;
using ::bioseq::minimizers::require_representable_minimizer_query_length;
using ::bioseq::minimizers::basic_sketch_collector;
using ::bioseq::minimizers::sketch_collector;
using ::bioseq::minimizers::admitted_sketch_collector;

/** Number of logical sequence bits represented by one alphabet symbol. */
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
 * Collect minimizers from an iterator-bounded sequence view.
 *
 * Resident and fragmented views share this scalar implementation. Ambiguous
 * nucleotide symbols reset both the rolling hash and minimizer window so no
 * k-mer spanning an unsupported symbol can be emitted.
 */
template<
	template<typename p_hash_alphabet> typename p_scalar_hasher_template,
	class p_minimizer_type,
	class p_sequence_view,
	typename p_iterator = decltype(std::declval<p_sequence_view&>().begin()),
    typename p_hash_value = std::uint64_t,
    class p_collector_type = basic_sketch_collector<p_minimizer_type>
>
requires (
	supports_scalar_hash<
        p_scalar_hasher_template,
        typename p_sequence_view::traits::alphabet_type,
        p_hash_value>
)
[[gnu::always_inline]]
inline void collect_minimizers_view(
    p_collector_type& collector,
	const p_sequence_view& sequence,
	const p_iterator& sequence_begin,
	const p_iterator& sequence_end,
	const std::size_t window_size,
	const std::size_t kmer_size
) {
    static const auto* region_description =
        bioseq::instrumentation::register_static_region("mm_sketch");
    auto region_scope = bioseq::instrumentation::region(region_description);

	using sequence_traits = typename p_sequence_view::traits;
	using alphabet_type = typename sequence_traits::alphabet_type;
	// Nucleotide ranks use minimap2's two-bit nt4 code space. The dna5
	// ambiguity symbol maps to the reset sentinel; other alphabets retain their
	// native ranks.
	static constexpr bool k_is_nucleotide =
        seqan3::nucleotide_alphabet<alphabet_type>;
	using hash_alphabet =
		std::conditional_t<k_is_nucleotide, seq::dna4, alphabet_type>;
	using hasher_type = typename p_scalar_hasher_template<hash_alphabet>::
        template scalar_hash<p_hash_value>;

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
            // The nt4 sentinel marks an ambiguous nucleotide and therefore a
			// hard rolling-window boundary.
			std::uint8_t code;
			bool is_ambiguous;
			if constexpr (k_is_nucleotide) {
				code = nt4_normalize<alphabet_type>(static_cast<std::uint8_t>(nucl));
				is_ambiguous = (code >= k_nt4_sentinel);
			} else {
				code = static_cast<std::uint8_t>(nucl);
				is_ambiguous = (logical_bits_per_symbol_v<p_sequence_view> > 2
                    && nucl >= 4);
			}
			if(is_ambiguous) {
				// Reset both state machines so a k-mer spanning the ambiguity can
				// never reach the output.
				buffer.push_back(p_minimizer_type::maximum());
				valid_length = 0u;
				// hasher_type's copy-assignment is deleted; reset its rolling state
				// in place (destroy + reconstruct) so the post-N k-mer is clean.
				hasher.~hasher_type();
				::new (static_cast<void*>(&hasher)) hasher_type(kmer_size);
			} else {
				const auto [canonical_hash, strand] = hasher.next(
                    static_cast<p_hash_value>(code),
                    static_cast<p_hash_value>(code));

				// NOTE: minimap2 skips "symmetric k-mers" (fwd == rc palindromes) by
				// `continue`ing past the push/update logic — the iteration disappears from
				// the minimizer window entirely. The SIMD path cannot mirror that without
				// per-lane pos tracking (different lanes see different palindromes), so to
				// keep scalar and SIMD in lock-step we let the palindrome iter through but
				// push `minimap_sketch_data::maximum()`. A max-valued sketch can never become a
				// window minimum, so the "don't know the strand" intent is preserved: the
				// palindromic k-mer never gets emitted as a minimizer. The difference is
				// only structural (window slot is consumed instead of skipped), which is
				// exactly what the SIMD path does with the equivalent IfThenElse mask.
				const auto kmer_span =
                    (valid_length + 1 < kmer_size && p_is_initializing)
                    ? valid_length + 1
                    : kmer_size;
				++valid_length;

				if (strand == sequence_strand_mode::equal) {
					buffer.push_back(p_minimizer_type::maximum());
				} else if (valid_length >= kmer_size) {
					// Span remains part of the parity calculation even though the
					// compact output stores only hash, position, and strand.
					(void)kmer_span;
					buffer.push_back(p_minimizer_type(
						/*kmer=*/canonical_hash,
						/*last_pos=*/sequence_position,
						/*strand=*/static_cast<std::uint64_t>(strand)));
				} else {
					buffer.push_back(p_minimizer_type::maximum());
				}
			}

			if ((valid_length == window_size + kmer_size - 1
                    && p_is_initializing)
                && !current_min.is_max()) { // first full window
				buffer.for_each_filled_value_unsafe(
                    [&collector, &current_min](
                        const auto&, const auto& candidate, const auto&) {
                    if (p_minimizer_type::does_sketch_match(
                            current_min, candidate)) {
                        collector.collect(candidate);
					}
				});
			}
			if (buffer.get_last_value() <= current_min) {
				if ((valid_length >= window_size + kmer_size
                        || !p_is_initializing)
                    && !current_min.is_max()) {
					collector.collect(current_min);
				}
				current_min_pos = buffer.get_last_write_position();
				current_min = buffer.get_last_value();
			} else if (buffer.get_last_write_position() == current_min_pos) {
				if ((valid_length >= window_size + kmer_size - 1
                        || !p_is_initializing)
                    && !current_min.is_max()) {
					collector.collect(current_min);
				}

				current_min.set_max();
				buffer.for_each_filled_value_unsafe(
                    [&current_min, &current_min_pos](
                        const auto& index,
                        const auto& candidate,
                        const auto&) {
					if (current_min >= candidate) {
						current_min = candidate;
						current_min_pos = index;
					}
				});

				if ((valid_length >= window_size + kmer_size - 1
                        || !p_is_initializing)
                    && !current_min.is_max()) { // write identical k-mers
					buffer.for_each_filled_value_unsafe(
                        [&collector, &current_min](
                            const auto&,
                            const auto& candidate,
                            const auto&) {
						if (p_minimizer_type::does_sketch_match(
                                current_min, candidate)) {
							collector.collect(candidate);
						}
					});
				}
			}
        }
    };

    // First kmers. Walk the prefix in INIT mode (until the first window
    // is fully populated), then the rest in non-INIT mode.
    collect_range(std::bool_constant<true>{});
    collect_range(std::bool_constant<false>{});

    if (!current_min.is_max()) {
		collector.collect(current_min);
	}
}

/** Re-export backend selection and sorted-pair contracts. */
using ::bioseq::minimizers::minimizer_sketch_backend;
using ::bioseq::minimizers::minimizer_sketch_backend_name;
using ::bioseq::minimizers::is_vulkan_minimizer_backend;
using ::bioseq::minimizers::minimizer_sorted_pair;

/** Collect one bounded lazy-sequence segment with the scalar backend. */
template <
    class p_minimizer_type,
    sequence_alphabet p_alphabet,
    class p_collector_type>
[[gnu::always_inline]] inline void collect_minimizers_segment_scalar(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size)
{
    if (lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        throw std::out_of_range(
            "minimizer base-pair segment exceeds lazy sequence");
    }
    auto begin_iter = sequence.begin();
    auto end_iter = sequence.begin();
    begin_iter += static_cast<std::ptrdiff_t>(lo_bp);
    end_iter += static_cast<std::ptrdiff_t>(hi_bp);
    collect_minimizers_view<minimap_int_hasher, p_minimizer_type>(
        collector,
        sequence,
        begin_iter,
        end_iter,
        window_size,
        kmer_size);
}

/** Number of packed storage units required by one sequence type. */
template <class p_sequence>
inline constexpr std::size_t letters_per_unit_v =
    std::remove_cvref_t<p_sequence>::traits::letters_per_unit;

/** Convert a base-pair count to packed storage units without truncation. */
template <class p_sequence>
[[nodiscard]] inline bioseq::storage::sequence_size_t unit_count_for_bp(
    bioseq::storage::sequence_size_t bp) noexcept
{
    constexpr bioseq::storage::sequence_size_t lpu =
        static_cast<bioseq::storage::sequence_size_t>(letters_per_unit_v<p_sequence>);
    return (bp + lpu - 1) / lpu;
}

/** Position field mask exposed as the canonical sequence-size type. */
template <class p_minimizer_type>
inline constexpr bioseq::storage::sequence_size_t
minimizer_pos_field_mask_v =
    static_cast<bioseq::storage::sequence_size_t>(p_minimizer_type::k_position_field_mask);

/** Whether every output position in a span fits the minimizer representation. */
template <class p_minimizer_type>
[[nodiscard]] inline bool collected_span_fits_minimizer_pos_field(
    bioseq::storage::sequence_size_t collected_bp) noexcept
{
    return collected_bp <= k_max_minimizer_sequence_length<p_minimizer_type>;
}

/** Collect minimizers from a concrete compact sequence range. */
template<
	template<typename p_hash_alphabet> typename p_scalar_hasher_template,
	class p_minimizer_type,
	supports_compact_sequence_range p_sequence_range,
	typename p_iterator = decltype(compact_sequence_range<p_sequence_range>().begin()),
	typename p_hash_value = std::uint64_t,
	class p_collector_type = basic_sketch_collector<p_minimizer_type>
>
requires (
	supports_scalar_hash<
        p_scalar_hasher_template,
        typename compact_sequence_range<p_sequence_range>::traits::alphabet_type,
        p_hash_value>
)
[[gnu::always_inline]]
inline void collect_minimizers(
    p_collector_type& collector,
	compact_sequence_range<p_sequence_range>& sequence,
	const p_iterator& sequence_begin,
	const p_iterator& sequence_end,
	const std::size_t window_size,
	const std::size_t kmer_size
) {
	collect_minimizers_view<
        p_scalar_hasher_template,
        p_minimizer_type,
        compact_sequence_range<p_sequence_range>,
        p_iterator,
        p_hash_value,
        p_collector_type>(
		collector,
        sequence,
        sequence_begin,
        sequence_end,
        window_size,
        kmer_size);
}

/** Compile-time window/k-mer bridge for compact sequence ranges. */
template <
    std::size_t p_window_size, std::size_t p_kmer_size,
    template<typename p_hash_alphabet> typename p_scalar_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(compact_sequence_range<p_sequence_range>().begin()),
    typename p_hash_value = std::uint64_t,
    class p_collector_type = basic_sketch_collector<p_minimizer_type>>
[[gnu::always_inline]]
inline void collect_minimizers_constexpr(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& sequence,
    const p_iterator& sequence_begin,
    const p_iterator& sequence_end)
{
    collect_minimizers<
        p_scalar_hasher_template,
        p_minimizer_type,
        p_sequence_range,
        p_iterator,
        p_hash_value,
        p_collector_type>(
        collector,
        sequence,
        sequence_begin,
        sequence_end,
        p_window_size,
        p_kmer_size);
}

} // namespace bioseq::simd
