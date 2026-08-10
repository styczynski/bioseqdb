/**
 * @file highway_minimap_int_kernel.cppm
 * @brief Fixed-target Highway minimap2 integer hasher.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_minimap_int;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.hasher_lexic;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

template<sequence_alphabet p_alphabet>
struct minimap_int_hasher
    : ::bioseq::simd::minimap_int_hasher<p_alphabet> {
public:
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /** Stateful SIMD minimap2 integer hash. */
    template<typename p_input_type, typename p_simd_tag>
    class simd_hash
        : public simd_hasher_base<p_alphabet, p_input_type, p_simd_tag> {
    public:
        using base_type =
            simd_hasher_base<p_alphabet, p_input_type, p_simd_tag>;
        using traits = typename base_type::traits;

    private:
        typename base_hasher_type::template simd_hash<
            p_input_type, p_simd_tag> base_hasher_;
        const typename traits::input_vector_type mask_;

    public:
        /** Initialize the lexicographic state and k-mer mask. */
        explicit simd_hash(
            const typename traits::input_simd_tag_type& simd_tag,
            std::size_t kmer_length)
            : base_hasher_(simd_tag, kmer_length),
              mask_(hn::Set(
                  simd_tag,
                  (static_cast<typename traits::input_type>(1)
                      << (2u * kmer_length)) - 1u)) {}

        /** Clamp a fixed scramble shift to the selected storage width. */
        template<std::size_t p_offset>
        static constexpr std::size_t k_shift =
            std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /** Consume one vector and apply the minimap2 integer permutation. */
        HWY_INLINE typename traits::hasher_return_type next(
            const typename traits::input_simd_tag_type& simd_tag,
            typename traits::input_vector_type incoming,
            typename traits::input_vector_type outgoing) {
            auto key = base_hasher_.next(
                simd_tag, std::move(incoming), std::move(outgoing));
            key = hn::And(
                hn::Add(hn::Not(key), hn::ShiftLeft<k_shift<21>>(key)), mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<24>>(key));
            key = hn::And(
                hn::Add(
                    hn::Add(key, hn::ShiftLeft<k_shift<3>>(key)),
                    hn::ShiftLeft<k_shift<8>>(key)),
                mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<14>>(key));
            key = hn::And(
                hn::Add(
                    hn::Add(key, hn::ShiftLeft<k_shift<2>>(key)),
                    hn::ShiftLeft<k_shift<4>>(key)),
                mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<28>>(key));
            return hn::And(
                hn::Add(key, hn::ShiftLeft<k_shift<31>>(key)), mask_);
        }

		// Forward to the underlying lexic hasher — `minimap_int_hasher` is a
		// deterministic post-hash on the lexic result, so its per-lane palindrome
		// status is exactly whatever the base hasher reports.
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> palindrome_mask(
            const p_simd_tag& simd_tag) const {
            return base_hasher_.palindrome_mask(simd_tag);
        }

		// Same forwarding rationale as get_palindrome_mask: the integer scramble is
		// applied to whichever of (forward, reverse_complement) the base hasher's
		// `Min` selected, so the per-lane strand decision is determined entirely by
		// the lexic state.
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> reverse_strand_mask(
            const p_simd_tag& simd_tag) const {
            return base_hasher_.reverse_strand_mask(simd_tag);
        }
    };

	// Highway sibling of `scramble`: the exact same seven-step integer mix,
	// applied to every canonical-kmer lane before two-stack window selection.
	// Keeping this beside the scalar definition makes scalar/SIMD drift visible
	// and removes the old per-emitted-minimizer scalar hash tail.
	template<class p_simd_tag>
	[[nodiscard, gnu::always_inline]] static HWY_INLINE hn::Vec<p_simd_tag> scramble_simd(
		p_simd_tag simd_tag,
		hn::Vec<p_simd_tag> key,
		hn::Vec<p_simd_tag> mask)
	{
		using lane_type = hn::TFromD<p_simd_tag>;
		constexpr size_t k_max_storage_shift = sizeof(lane_type) * 8 - 1;
		constexpr auto clamp = [](size_t shift) constexpr -> size_t {
			return shift > k_max_storage_shift ? k_max_storage_shift : shift;
		};
		key = hn::And(
			hn::Add(hn::Not(key), hn::ShiftLeft<clamp(21)>(key)), mask);
		key = hn::Xor(key, hn::ShiftRight<clamp(24)>(key));
		key = hn::And(
			hn::Add(
				hn::Add(key, hn::ShiftLeft<clamp(3)>(key)),
				hn::ShiftLeft<clamp(8)>(key)),
			mask);
		key = hn::Xor(key, hn::ShiftRight<clamp(14)>(key));
		key = hn::And(
			hn::Add(
				hn::Add(key, hn::ShiftLeft<clamp(2)>(key)),
				hn::ShiftLeft<clamp(4)>(key)),
			mask);
		key = hn::Xor(key, hn::ShiftRight<clamp(28)>(key));
		key = hn::And(
			hn::Add(key, hn::ShiftLeft<clamp(31)>(key)), mask);
		(void)simd_tag;
		return key;
	}
};
static_assert(
    ::bioseq::simd::supports_scalar_hash<minimap_int_hasher, seq::dna4>);
static_assert(supports_simd_hash<minimap_int_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
