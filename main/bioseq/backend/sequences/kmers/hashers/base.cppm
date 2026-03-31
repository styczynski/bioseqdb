module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq_sequences.kmers.hashers.base;

import mlogging;
import sequan_compat;
import bioseq_sequences;
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
void run_fall_hashers_on_sequence(
    const compact_sequence_range<R>& seq,
    const size_t kmer_length
) {
    using seq_traits = compact_sequence_range<R>::traits;
    hn::ScalableTag<I> d;
    typename H<typename seq_traits::alphabet_t>::template scalar_hash<I> h_scalar(kmer_length);
    typename H<typename seq_traits::alphabet_t>::template simd_hash<I, decltype(d)> h_simd(d, kmer_length);
    
    std::vector<I> ret_scalar;
    std::vector<std::pair<size_t, I>> ret_simd;

    ret_scalar.reserve(seq.bp_size());
    ret_simd.reserve(seq.bp_size());

    for (auto& nucl : seq.template iterable<BP>()) {
        const auto& [hash_val, _] = h_scalar.next(static_cast<I>(nucl), static_cast<I>(nucl));
        ret_scalar.push_back(hash_val);
    }

    size_t pos = 0;
    iterate_sequence_bp_simd_offsets(seq, seq.begin(), seq.end(), kmer_length, [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d, auto& mask, auto&& incoming, auto outgoing_mask, auto outgoing){ //auto&& bp_del1
        const auto hash_val = h_simd.next(incoming, outgoing);
        const auto& values = extract_lanes(d, hash_val);
        size_t index = 0;
        for (auto& value : values) {
            ret_simd.push_back(std::make_pair(offsets[index] + pos, value));
            ++index;
        }
        ++pos;
    }, 0);

    std::sort(ret_simd.begin(), ret_simd.end());
    ret_simd.erase(std::unique(ret_simd.begin(), ret_simd.end()), ret_simd.end());

    JustLog("SCALAR = {}", ret_scalar);
    JustLog("SIMD = {}", ret_simd);
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


}
HWY_AFTER_NAMESPACE();

}
