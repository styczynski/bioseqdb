/** Canonical generator input for the exact lexic minimizer hasher. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_lexic;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
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
			// Cast letter_complement to `input_t` (the state-width type, e.g. uint64_t) BEFORE
			// the shift. `traits::letter_complement` returns `traits::storage_unit` which is
			// derived from the default compact_sequence_range<alphabet>::traits — that default
			// type is uint8_t for dna4/dna5. Shifting a uint8_t (promoted to int) by
			// `rev_shift` invokes UB when rev_shift >= 32 (e.g. dna5 k=15 → rev_shift=42),
			// which on AArch64 produces garbage results and broke scalar≠SIMD parity for
			// dna5 × uint64_t. The input_t promotion keeps the shift in the wide state type.
			state_reverse_compl = (state_reverse_compl >> traits::bits_per_char) | (static_cast<input_t>(traits::letter_complement(outgoing)) << rev_shift);
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

		// Compile-time complement table: index = rank, value = complement(rank).
		// We cannot use `Xor(rank, char_mask)` in general — that works for dna4
		// because its complement pairs are {0,3} and {1,2} (xor 3 swaps them),
		// but for dna5 the pairs are {0 A ↔ 4 T}, {1 C ↔ 2 G}, {3 N self} and no
		// single XOR mask covers all three cases. A static lookup table does.
		static constexpr auto build_compl_lut() {
			std::array<input_t, traits::alphabet_size> lut{};
			for (size_t r = 0; r < traits::alphabet_size; ++r) {
				lut[r] = static_cast<input_t>(traits::letter_complement(
					static_cast<typename traits::storage_unit>(r)));
			}
			return lut;
		}
		static constexpr std::array<input_t, traits::alphabet_size> compl_lut = build_compl_lut();

		// Detect XOR-complement alphabets at compile time. dna4 is the
		// production case: complement pairs are {0,3} and {1,2}, both of
		// which are flipped by XOR with the value `alphabet_size - 1 = 3`.
		// One vector XOR is dramatically cheaper than the (alphabet_size-1)
		// chained IfThenElse cascade the LUT path falls back to — for dna4
		// this is one of the dominant per-iteration costs of mm_sketch_simd.
		// dna5 fails this predicate ({0,4} flips ok, {1,2} flips ok, {3,3}
		// — N — must stay at 3 but `3 ^ 4 = 7`) so the LUT remains the
		// fallback for non-power-of-2 alphabets.
		// See REPORT_POOR_BENCHMARK_PERFORMANCE_AND_LOW_QUALITY_SIMD_MM_V_2.md
		// for measurement notes.
		static constexpr bool is_xor_complement = []() constexpr {
			using sw = typename traits::storage_unit;
			constexpr sw xor_mask = static_cast<sw>(traits::alphabet_size - 1);
			for (size_t r = 0; r < traits::alphabet_size; ++r) {
				const sw expected = static_cast<sw>(r) ^ xor_mask;
				const sw actual = static_cast<sw>(traits::letter_complement(static_cast<sw>(r)));
				if (expected != actual) return false;
			}
			return true;
		}();

		// Compute `complement(outgoing)` per lane. For dna4 a single Xor with
		// (alphabet_size - 1) suffices; for non-XOR alphabets we fall back to
		// the IfThenElse cascade over the compile-time LUT. The branch is
		// `if constexpr` so each instantiation gets exactly one code path.
		[[always_inline]] static constexpr typename traits::input_vec_t
		apply_complement(const typename traits::input_d_t& d, typename traits::input_vec_t rank) {
			if constexpr (is_xor_complement) {
				return hn::Xor(rank, hn::Set(d,
					static_cast<input_t>(traits::alphabet_size - 1)));
			} else {
				typename traits::input_vec_t result = hn::Set(d, compl_lut[traits::alphabet_size - 1]);
				for (int r = static_cast<int>(traits::alphabet_size) - 2; r >= 0; --r) {
					result = hn::IfThenElse(
						hn::Eq(rank, hn::Set(d, static_cast<input_t>(r))),
						hn::Set(d, compl_lut[r]),
						result
					);
				}
				return result;
			}
		}

	public:
		explicit simd_hash(
			const traits::input_d_t& d,
			const size_t& kmer_length
		): mask(hn::Set(d, traits::template kmer_mask<input_t>(std::min(kmer_length, traits::max_kmer_length)))), state_forward(hn::Zero(d)), state_reverse_compl(hn::Zero(d)), rev_shift(traits::bits_per_char * (std::min(kmer_length, traits::max_kmer_length)-1)) {};

		constexpr traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
			state_forward = hn::And(hn::Or(hn::ShiftLeft<traits::bits_per_char>(state_forward), incoming), mask);
			const typename traits::input_vec_t new_vec = hn::ShiftLeftSame(apply_complement(d, outgoing), rev_shift);
			state_reverse_compl = hn::Or(hn::ShiftRight<traits::bits_per_char>(state_reverse_compl), new_vec);
			return hn::Min(state_forward, state_reverse_compl);
		}

		// Returns per-lane mask where `true` means the current k-mer is palindromic
		// (state_forward == state_reverse_compl). Minimap2 / scalar skips palindromes
		// because "we don't know its strand". For SIMD parity the caller replaces
		// palindromic lanes' hash with max_val so they never become window minima —
		// matching the scalar's post-palindrome-push-max behaviour without requiring
		// per-lane pos tracking.
		[[nodiscard]] constexpr hn::Mask<d_t> get_palindrome_mask(const d_t& d) const {
			return hn::Eq(state_forward, state_reverse_compl);
		}

		// Returns per-lane mask where `true` means the reverse-complement state is the
		// minimum (so `min(forward, rc)` chose the rc — strand=1 / SEQUENCE_STRAND_REVERSE_COMPLEMENT)
		// and `false` means the forward state was the minimum (strand=0 / SEQUENCE_STRAND_FORWARD).
		// Mirrors `select_min_hash_strand`'s `forward < reverse_complement ? FORWARD : REVERSE_COMPLEMENT`
		// classification — palindromic lanes (forward == reverse_complement) report `false` here but
		// the caller separately masks them out via `get_palindrome_mask` and pushes a sentinel hash.
		[[nodiscard]] constexpr hn::Mask<d_t> get_strand_mask(const d_t& d) const {
			return hn::Lt(state_reverse_compl, state_forward);
		}


	};

	// Static "post-lex" scramble. Identity for the lexic hasher — its
	// `next()` already returns the canonical lex hash (Min(forward, RC)),
	// and downstream consumers that compute lex inline + apply a hasher's
	// scramble at emit time can call this uniformly across hasher variants
	// without conditional compilation.
	//
	// The two-stack minimizer collector uses this entry point (via
	// `simd_hash_t<alphabet>::template scramble<input_t>`) so that passing
	// `lexic_hasher` as the `simd_hash_t` template argument actually
	// produces a lex-only emit, mirroring the parameter's intent.
	template<typename input_t>
	[[nodiscard]] static constexpr input_t scramble(
		const input_t& lex_kmer,
		[[maybe_unused]] const input_t& kmer_mask
	) {
		return lex_kmer;
	}

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};
static_assert(supports_scalar_hash<lexic_hasher, seq::dna4>);
static_assert(supports_simd_hash<lexic_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
