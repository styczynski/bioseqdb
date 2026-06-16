module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq.sequences.kmers.hashers.minimap_int;

import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.common.circular_append_buffer;
import bioseq.simd.utils;

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

			key = (~key + (key << shift<21>)) & mask;
			key = key ^ key >> shift<24>;
			key = ((key + (key << shift<3>)) + (key << shift<8>)) & mask;
			key = key ^ key >> shift<14>;
			key = ((key + (key << shift<2>)) + (key << shift<4>)) & mask;
			key = key ^ key >> shift<28>;
			key = (key + (key << shift<31>)) & mask;

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

			key = hn::And(hn::Add(hn::Not(key), hn::ShiftLeft<shift<21>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<24>>(key));
			key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<shift<3>>(key)), hn::ShiftLeft<shift<8>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<14>>(key));
			key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<shift<2>>(key)), hn::ShiftLeft<shift<4>>(key)), mask);
			key = hn::Xor(key, hn::ShiftRight<shift<28>>(key));
			key = hn::And(hn::Add(key, hn::ShiftLeft<shift<31>>(key)), mask);

			return key;
		}

		// Forward to the underlying lexic hasher — `minimap_int_hasher` is a
		// deterministic post-hash on the lexic result, so its per-lane palindrome
		// status is exactly whatever the base hasher reports.
		[[nodiscard]] constexpr hn::Mask<d_t> get_palindrome_mask(const d_t& d) const {
			return base_hasher.get_palindrome_mask(d);
		}

		// Same forwarding rationale as get_palindrome_mask: the integer scramble is
		// applied to whichever of (forward, reverse_complement) the base hasher's
		// `Min` selected, so the per-lane strand decision is determined entirely by
		// the lexic state.
		[[nodiscard]] constexpr hn::Mask<d_t> get_strand_mask(const d_t& d) const {
			return base_hasher.get_strand_mask(d);
		}


	};

	// Static "post-lex" scramble — the seven-step integer mix from the
	// minimap_int scalar_hash::next() body, applied to a precomputed lex
	// kmer at emit time.
	//
	// Used by collectors that compute lex inline (cheap golden-ratio mix
	// for window ordering) and apply the canonical minimap_int scramble
	// only when a window minimum is emitted. The bit-pattern is identical
	// to scalar_hash<input_t>::next's emit value because the body below is
	// the literal copy of that body's scramble half (the lex-rolling half
	// stays in the caller).
	//
	// Shifts are clamped via `clamp` so that on narrow `input_t` (e.g.
	// uint8_t) we don't shift past `sizeof(input_t)*8 - 1` and invoke UB.
	// The same clamp pattern is used in the simd_hash::next body and in
	// the inline scramble lambdas this method replaces.
	template<typename input_t>
	[[nodiscard]] static constexpr input_t scramble(
		const input_t& lex_kmer,
		const input_t& kmer_mask
	) {
		constexpr size_t storage_max_shift = sizeof(input_t) * 8 - 1;
		constexpr auto clamp = [](size_t s) constexpr -> size_t {
			return s > storage_max_shift ? storage_max_shift : s;
		};
		input_t key = lex_kmer;
		key = (~key + (key << clamp(21))) & kmer_mask;
		key = key ^ (key >> clamp(24));
		key = ((key + (key << clamp(3))) + (key << clamp(8))) & kmer_mask;
		key = key ^ (key >> clamp(14));
		key = ((key + (key << clamp(2))) + (key << clamp(4))) & kmer_mask;
		key = key ^ (key >> clamp(28));
		key = (key + (key << clamp(31))) & kmer_mask;
		return key;
	}

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};
static_assert(supports_scalar_hash<minimap_int_hasher, seq::dna4>);
static_assert(supports_simd_hash<minimap_int_hasher, seq::dna4>);

}
HWY_AFTER_NAMESPACE();

}
