module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq_sequences.kmers.hashers.minimap_int;

import bioseq_sequences.kmers.hashers.base;
import bioseq_sequences.kmers.hashers.lexic;

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
struct minimap_int_hasher {
public:
	using base_hasher_t = lexic_hasher<alphabet_t>;

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
		typename base_hasher_t::template scalar_hash<input_t> base_hasher;
	public:
		explicit scalar_hash(
			const size_t& kmer_length
		): base_hasher(kmer_length) {};

		constexpr traits::hasher_return_t next(traits::input_t incoming, traits::input_t outgoing) {
			auto [key, base_strand] = base_hasher.next(std::move(incoming), std::move(outgoing));

			key = (~key + (key << 21));
			key = key ^ key >> 24;
			key = ((key + (key << 3)) + (key << 8));
			key = key ^ key >> 14;
			key = ((key + (key << 2)) + (key << 4));
			key = key ^ key >> 28;
			key = (key + (key << 31));

			return std::make_pair(static_cast<input_t>(key), base_strand);
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
		typename base_hasher_t::template simd_hash<input_t, d_t> base_hasher;
	public:
		explicit simd_hash(
			const traits::input_d_t& d,
			const size_t& kmer_length
		): base_hasher(d, kmer_length) {};

		constexpr traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
			auto key = base_hasher.next(d, std::move(incoming), std::move(outgoing));

			key = hn::Add(hn::Not(key), hn::ShiftLeft<21>(key));
			key = hn::Xor(key, hn::ShiftRight<24>(key));
			key = hn::Add(hn::Add(key, hn::ShiftLeft<3>(key)), hn::ShiftLeft<8>(key));
			key = hn::Xor(key, hn::ShiftRight<14>(key));
			key = hn::Add(hn::Add(key, hn::ShiftLeft<2>(key)), hn::ShiftLeft<4>(key));
			key = hn::Xor(key, hn::ShiftRight<28>(key));
			key = hn::Add(key, hn::ShiftLeft<31>(key));

			return key;
		}
        

	};

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};
static_assert(supports_scalar_hash<minimap_int_hasher, seq::dna4>);
static_assert(supports_simd_hash<minimap_int_hasher, seq::dna4>);

}
HWY_AFTER_NAMESPACE();

}
