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
		const traits::input_t mask;
	public:
		explicit scalar_hash(
			const size_t& kmer_length
		): base_hasher(kmer_length), mask((static_cast<traits::input_t>(1)<<2*kmer_length) - 1) {};

		template<size_t offset>
        static constexpr size_t shift = std::min(offset, sizeof(input_t) * 8 - 1);

		constexpr traits::hasher_return_t next(traits::input_t incoming, traits::input_t outgoing) {
			auto [key, base_strand] = base_hasher.next(std::move(incoming), std::move(outgoing));
			auto rr = std::make_pair(static_cast<input_t>(key), base_strand);

			key = (~key + (key << shift<21>)) & mask;
			key = key ^ key >> shift<24>;
			key = ((key + (key << shift<3>)) + (key << shift<8>)) & mask;
			key = key ^ key >> shift<14>;
			key = ((key + (key << shift<2>)) + (key << shift<4>)) & mask;
			key = key ^ key >> shift<28>;
			key = (key + (key << shift<31>)) & mask;

			JustLog("{} -> {}", rr.first, key);
			return std::make_pair(key, base_strand);
			//return std::make_pair(static_cast<input_t>(key), base_strand);
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
		const traits::input_vec_t mask;
	public:
		explicit simd_hash(
			const traits::input_d_t& d,
			const size_t& kmer_length
		): base_hasher(d, kmer_length), mask(
			hn::Set(d, (static_cast<traits::input_t>(1)<<2*kmer_length) - 1)
		) {};

		template<size_t offset>
        static constexpr size_t shift = std::min(offset, sizeof(input_t) * 8 - 1);

		constexpr traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
			auto key = base_hasher.next(d, std::move(incoming), std::move(outgoing));
			const auto rr = key;

			key = hn::And(hn::Add(hn::Not(key), hn::ShiftLeft<shift<21>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<24>>(key));
			key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<shift<3>>(key)), hn::ShiftLeft<shift<8>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<14>>(key));
			key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<shift<2>>(key)), hn::ShiftLeft<shift<4>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<28>>(key));
			key = hn::And(hn::Add(key, hn::ShiftLeft<shift<31>>(key)), mask);

			JustLog("{} -> {}", extract_lanes<input_t>(d, rr)[0], extract_lanes<input_t>(d, key)[0]);

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
