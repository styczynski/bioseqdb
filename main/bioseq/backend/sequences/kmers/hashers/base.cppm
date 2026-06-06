module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <array>
#include <cstdint>
#include <string>
#include <functional>

export module bioseq_sequences.kmers.hashers.base;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import algo.circular_append_buffer;
import bioseq_simd.utils;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// hasher
template<
    typename p_derived_t,
    sequence_alphabet p_alphabet_t,
    typename p_input_t
>
struct hasher_base {
    struct traits : public compact_sequence_range<p_alphabet_t>::compact_sequence_range_base::traits {
        using input_t = p_input_t;

        using hasher_return_t = std::pair<input_t, sequence_strand_mode>;

        static constexpr size_t max_kmer_length = sizeof(p_input_t) * 8 / compact_sequence_range<p_alphabet_t>::compact_sequence_range_base::traits::bits_per_char;

        template <typename input_t_like>
        [[always_inline]] static constexpr hasher_return_t select_min_hash_strand(
            const input_t_like&& forward, 
            const input_t_like&& reverse_complement
        ) noexcept {
            if (forward == reverse_complement) [[unlikely]] {
                return std::make_pair(static_cast<input_t>(forward), SEQUENCE_STRAND_EQUAL);
            } else if (forward < reverse_complement) {
                return std::make_pair(static_cast<input_t>(forward), SEQUENCE_STRAND_FORWARD);
            } else {
                return std::make_pair(static_cast<input_t>(reverse_complement), SEQUENCE_STRAND_REVERSE_COMPLEMENT);
            }
        }


    };
};


template<template<typename H_A> typename H, typename A, typename I = uint64_t, typename hasher_t = H<A>::template scalar_hash<I>>
concept supports_scalar_hash = sequence_alphabet<A> && requires (hasher_t& hasher, I&& incoming) {
	{ hasher.next(std::move(incoming), std::move(incoming)) } -> std::same_as<typename hasher_t::traits::hasher_return_t>;
};

template<template<typename H_A> typename H, typename A, typename I = uint64_t, typename D = hn::ScalableTag<I>, typename hasher_t = H<A>::template simd_hash<I, D>>
concept supports_simd_hash = sequence_alphabet<A> && requires (hasher_t& hasher, D& d, hn::Vec<D>&& incoming) {
	{ hasher.next(d, std::move(incoming), std::move(incoming)) } -> std::same_as<typename hasher_t::traits::hasher_return_t>;
};

template<
    typename I,
    template<typename H_A> typename H,
    supports_compact_sequence_range R
>
auto run_all_hashers_on_sequence(
    const compact_sequence_range<R>& seq,
    const size_t kmer_length
) {
    using seq_traits = compact_sequence_range<R>::traits;
    hn::ScalableTag<I> d;
    typename H<typename seq_traits::alphabet_type>::template scalar_hash<I> h_scalar(kmer_length);
    typename H<typename seq_traits::alphabet_type>::template simd_hash<I, decltype(d)> h_simd(d, kmer_length);
    
    std::vector<std::pair<size_t, I>> ret_scalar;
    std::vector<std::pair<size_t, I>> ret_simd;

    ret_scalar.reserve(seq.bp_size());
    ret_simd.reserve(seq.bp_size());

    size_t pos = 0;
    for (auto const& nucl : seq.template iterable<BP>()) {
        const auto& [hash_val, _] = h_scalar.next(static_cast<I>(nucl), static_cast<I>(nucl));
        //JustLog("pos = {} / {}", pos, seq.bp_size());
        if (pos >= kmer_length) {
            ret_scalar.push_back(std::make_pair(pos, hash_val));
        }
        ++pos;
    }

    pos = 0;
    iterate_sequence_bp_simd_offsets(seq, seq.begin(), seq.end(), kmer_length, [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d, auto& mask, auto&& incoming, auto outgoing_mask, auto outgoing){ //auto&& bp_del1
        const auto hash_val = h_simd.next(d, incoming, outgoing);
        if (pos >= kmer_length) {
            const auto& values = extract_lanes<I>(d, hash_val);
            const auto& mask_values = extract_lanes<I>(d, mask);
            size_t index = 0;
            for (auto& value : values) {
                // JustLog("--> pos={} offsets[{}]={} <{}> -> {}", pos, index, offsets[index] * seq_traits::letters_per_unit, mask_values[index] != 0, offsets[index] * seq_traits::letters_per_unit + pos);
                if (mask_values[index]) {
                    ret_simd.push_back(std::make_pair(offsets[index] * seq_traits::letters_per_unit + pos, value));
                }
                ++index;
            }
        }
        ++pos;
    }, [](auto const& _offsets, auto& d, auto& mask){}, 0);

    std::sort(ret_simd.begin(), ret_simd.end());
    //ret_simd.erase(std::unique(ret_simd.begin(), ret_simd.end()), ret_simd.end());

    // JustLog("simd = {}", ret_simd);
    // JustLog("scalar = {}", ret_scalar);

    return std::make_tuple(
        ret_simd,
        ret_scalar
    );
};

// hasher
template<
    typename p_derived_t,
    sequence_alphabet p_alphabet_t,
    typename p_input_t,
	typename p_d_t
>
struct simd_hasher_base : hasher_base<p_derived_t, p_alphabet_t, p_input_t> {
    struct traits : public simd_hasher_base<p_derived_t, p_alphabet_t, p_input_t, p_d_t>::hasher_base::traits {
        using input_d_t = p_d_t;
		using input_vec_t = hn::Vec<input_d_t>;
		using hasher_return_t = input_vec_t;
    };
};

// DOC0286 §10.2 — minimap2-compatible nt4 normalization. SeqAn nucleotide
// ranks are alphabet-specific (dna5: A=0,C=1,G=2,N=3,T=4), but minimap2's
// `seq_nt4_table` maps A/C/G/T → 0..3 and N/unknown → sentinel 4, and
// `mm_sketch` resets the rolling k-mer/window on any code ≥ 4. Hashing raw
// SeqAn dna5 ranks is therefore wrong (it would reset on T and hash N). This
// helper maps each alphabet rank to its minimap2 nt4 code so the scalar,
// two-stack-nthash, and persisted-index minimizer paths all agree with
// minimap2: dna4 is the identity (no N), dna5 swaps T↔3 and sends N→4. The
// nt4 codes are 2-bit, so callers hash them through the dna4 (2-bit, 3^code
// complement) hasher, matching minimap2's packing exactly.
constexpr std::uint8_t kNt4Sentinel = 4;

/// minimap2 seq_nt4_table on a single ASCII base (A/C/G/T → 0..3, else 4).
[[nodiscard]] constexpr std::uint8_t char_to_nt4(char base) noexcept {
    switch (base) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default:            return kNt4Sentinel;  // N / unknown → reset sentinel
    }
}

/// Compile-time rank→nt4 table for an alphabet, derived from each rank's
/// canonical char run through `char_to_nt4`. dna4 → {0,1,2,3}; dna5 →
/// {0,1,2,4,3}.
template <sequence_alphabet alphabet_t>
[[nodiscard]] constexpr auto make_nt4_lut() noexcept {
    std::array<std::uint8_t, seqan3::alphabet_size<alphabet_t>> lut{};
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<alphabet_t>; ++rank) {
        alphabet_t letter{};
        letter.assign_rank(static_cast<seqan3::alphabet_rank_t<alphabet_t>>(rank));
        lut[rank] = char_to_nt4(letter.to_char());
    }
    return lut;
}

template <sequence_alphabet alphabet_t>
inline constexpr auto nt4_lut_v = make_nt4_lut<alphabet_t>();

/// Map a raw alphabet rank to its minimap2 nt4 code (0..3, or `kNt4Sentinel`
/// for ambiguity). Identity for dna4; T↔3 / N→4 for dna5.
template <sequence_alphabet alphabet_t>
[[nodiscard, gnu::always_inline]] constexpr std::uint8_t
nt4_normalize(std::uint8_t rank) noexcept {
    return nt4_lut_v<alphabet_t>[rank];
}


}
HWY_AFTER_NAMESPACE();

}
