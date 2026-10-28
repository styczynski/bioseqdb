/**
 * @file highway_base_kernel.cppm
 * @brief Adds Highway vector types to the shared minimizer hasher interface.
 * The generator compiles these types for each  selected CPU target.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_base;

import std;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::simd::char_to_nt4;
using ::bioseq::simd::hasher_base;
using ::bioseq::simd::k_nt4_sentinel;
using ::bioseq::simd::make_nt4_lut;
using ::bioseq::simd::k_nt4_lut;
using ::bioseq::simd::nt4_normalize;
using ::bioseq::simd::supports_scalar_hash;

/** Exact nucleotide sketches retain nt4 semantics; proteins use every native rank. */
template<sequence_alphabet Alphabet>
inline constexpr std::size_t exact_rank_limit = seqan3::nucleotide_alphabet<Alphabet>
    ? seqan3::alphabet_size<seq::dna4> : seqan3::alphabet_size<Alphabet>;

/** Normalize only nucleotide exact sketches through the shared alphabet-derived map. */
template<sequence_alphabet Alphabet, class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> exact_hash_rank(D tag, hn::Vec<D> ranks) {
    if constexpr (!seqan3::nucleotide_alphabet<Alphabet>) {
        return ranks;
    } else {
        constexpr bool identity = [] {
            for (std::size_t rank = 0u; rank < seqan3::alphabet_size<Alphabet>; ++rank)
                if (k_nt4_lut<Alphabet>[rank] != rank) return false;
            return true;
        }();
        if constexpr (identity) return ranks;
        else {
            using lane_type = hn::TFromD<D>;
            auto mapped = hn::Set(tag, static_cast<lane_type>(k_nt4_sentinel));
            for (std::size_t rank = 0u; rank < seqan3::alphabet_size<Alphabet>; ++rank)
                mapped = hn::IfThenElse(hn::Eq(ranks, hn::Set(tag, static_cast<lane_type>(rank))),
                    hn::Set(tag, static_cast<lane_type>(k_nt4_lut<Alphabet>[rank])), mapped);
            return mapped;
        }
    }
}

/** @brief Adds SIMD vector types to the scalar hasher's alphabet traits. */
template <sequence_alphabet p_alphabet, typename p_input_type, typename p_simd_tag>
struct simd_hasher_base : hasher_base<p_alphabet, p_input_type> {
    /** @brief Scalar policy reused for alphabet limits and strand selection. */
    using scalar_base_type = hasher_base<p_alphabet, p_input_type>;

    /** @brief Vector and result types for this Highway target. */
    struct traits : public scalar_base_type::traits {
        /** @brief Highway descriptor for the SIMD lane type and count. */
        using input_simd_tag_type = p_simd_tag;
        /** @brief SIMD register holding one packed k-mer code per lane. */
        using input_vector_type = hn::Vec<input_simd_tag_type>;
        /** @brief Vector of hashes returned by simd_hash::next. */
        using hasher_return_type = input_vector_type;
    };
};

/** @brief Requires a SIMD hasher whose next method returns its declared vector type. */
template <template <typename> typename p_hasher_template,
          typename p_alphabet,
          typename p_input_type = std::uint64_t,
          typename p_simd_tag = hn::ScalableTag<p_input_type>,
          typename p_simd_hasher_type =
              p_hasher_template<p_alphabet>::template simd_hash<p_input_type, p_simd_tag>>
concept supports_simd_hash =
    ::sequence_alphabet<p_alphabet> &&
    requires(p_simd_hasher_type& hasher, p_simd_tag& simd_tag, hn::Vec<p_simd_tag>&& incoming) {
        {
            hasher.next(simd_tag, std::move(incoming))
        } -> std::same_as<typename p_simd_hasher_type::traits::hasher_return_type>;
    };

} // namespace target
} // namespace bioseq::simd
