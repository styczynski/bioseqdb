module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq_sequences.kmers.hashers.lexic;

import bioseq_sequences.kmers.hashers.base;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import algo.circular_append_buffer;
import bioseq_simd.utils;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

template<sequence_alphabet alphabet_t>
struct lexic_hasher {
public:
	//using traits = compact_sequence_range<alphabet_t>::compact_sequence_range_base::traits;

	// scalar
	template<typename input_t>
	class scalar_hash : public hasher_base<
        scalar_hash<input_t>,
        alphabet_t,
        input_t
    >{
	public:
        using traits = scalar_hash<input_t>::hasher_base::traits;
	private:
		input_t mask;
		size_t rev_shift;
		input_t state_forward;
		input_t state_reverse_compl;
	public:
		explicit scalar_hash(
			const size_t& kmer_length
		): mask(traits::template kmer_mask<input_t>(std::min(kmer_length, traits::max_kmer_length))), state_forward(0), state_reverse_compl(0), rev_shift(traits::bits_per_char * (std::min(kmer_length, traits::max_kmer_length)-1)) {};

		constexpr traits::hasher_return_t next(traits::input_t incoming, traits::input_t outgoing) {
			state_forward = (state_forward << traits::bits_per_char | incoming) & mask;
			state_reverse_compl = (state_reverse_compl >> traits::bits_per_char) | (traits::letter_complement(outgoing)) << rev_shift;
            return traits::select_min_hash_strand(std::move(state_forward), std::move(state_reverse_compl));
		}
	};

	template<typename input_t, typename d_t>
	class simd_hash : public simd_hasher_base<
        simd_hash<input_t, d_t>,
        alphabet_t,
        input_t,
		d_t
    >{
	public:
        using traits = simd_hash<input_t, d_t>::simd_hasher_base::traits;
	private:
		traits::input_vec_t mask;
		size_t rev_shift;
		traits::input_vec_t state_forward;
		traits::input_vec_t state_reverse_compl;
		const traits::input_vec_t compl_mask;
	public:
		explicit simd_hash(
			const traits::input_d_t& d,
			const size_t& kmer_length
		): mask(hn::Set(d, traits::template kmer_mask<input_t>(std::min(kmer_length, traits::max_kmer_length)))), state_forward(hn::Zero(d)), state_reverse_compl(hn::Zero(d)), rev_shift(traits::bits_per_char * (std::min(kmer_length, traits::max_kmer_length)-1)), compl_mask(hn::Set(d, traits::char_mask)) {};

		constexpr traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
			state_forward = hn::And(hn::Or(hn::ShiftLeft<traits::bits_per_char>(state_forward), incoming), mask);
			const typename traits::input_vec_t new_vec = hn::ShiftLeftSame(hn::Xor(outgoing, compl_mask), rev_shift);
			state_reverse_compl = hn::Or(hn::ShiftRight<traits::bits_per_char>(state_reverse_compl), new_vec);
			return hn::Min(state_forward, state_reverse_compl);
		}
        

	};

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};
static_assert(supports_scalar_hash<lexic_hasher, seq::dna4>);
static_assert(supports_simd_hash<lexic_hasher, seq::dna4>);

}
HWY_AFTER_NAMESPACE();

}
