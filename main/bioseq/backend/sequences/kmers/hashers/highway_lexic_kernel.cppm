/** Canonical generator input for the exact lexic minimizer hasher. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_lexic;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

template<sequence_alphabet alphabet_t>
struct lexic_hasher : ::bioseq::simd::lexic_hasher<alphabet_t> {
public:
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
#if HWY_TARGET_IS_SVE || HWY_TARGET == HWY_RVV
		// SVE and RVV vectors are sizeless C++ types: they are valid locals and
		// function arguments, but may not be class data members. Keep the rolling
		// state in ordinary lane storage and load/store at the callable boundary.
		// This is a target-local storage fallback, not a change to the hash
		// recurrence or lane ordering.
		static constexpr size_t lane_capacity = hn::MaxLanes(d_t{});
		input_t mask_value;
		std::array<input_t, lane_capacity> state_forward_storage{};
		std::array<input_t, lane_capacity> state_reverse_compl_storage{};
#else
		traits::input_vec_t mask;
		traits::input_vec_t state_forward;
		traits::input_vec_t state_reverse_compl;
#endif
		size_t rev_shift;

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
		):
#if HWY_TARGET_IS_SVE || HWY_TARGET == HWY_RVV
		  mask_value(traits::template kmer_mask<input_t>(
			std::min(kmer_length, traits::max_kmer_length))),
#else
		  mask(hn::Set(d, traits::template kmer_mask<input_t>(
			std::min(kmer_length, traits::max_kmer_length)))),
		  state_forward(hn::Zero(d)),
		  state_reverse_compl(hn::Zero(d)),
#endif
		  rev_shift(traits::bits_per_char
			* (std::min(kmer_length, traits::max_kmer_length) - 1)) {
			(void)d;
		}

		HWY_INLINE traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
#if HWY_TARGET_IS_SVE || HWY_TARGET == HWY_RVV
			auto state_forward = hn::LoadU(
				d, state_forward_storage.data());
			auto state_reverse_compl = hn::LoadU(
				d, state_reverse_compl_storage.data());
			const auto mask = hn::Set(d, mask_value);
			state_forward = hn::And(
				hn::Or(
					hn::ShiftLeft<traits::bits_per_char>(state_forward),
					incoming),
				mask);
			const auto new_vec = hn::ShiftLeftSame(
				apply_complement(d, outgoing), rev_shift);
			state_reverse_compl = hn::Or(
				hn::ShiftRight<traits::bits_per_char>(state_reverse_compl),
				new_vec);
			hn::StoreU(
				state_forward, d, state_forward_storage.data());
			hn::StoreU(
				state_reverse_compl,
				d,
				state_reverse_compl_storage.data());
			return hn::Min(state_forward, state_reverse_compl);
#else
			state_forward = hn::And(hn::Or(hn::ShiftLeft<traits::bits_per_char>(state_forward), incoming), mask);
			const typename traits::input_vec_t new_vec = hn::ShiftLeftSame(apply_complement(d, outgoing), rev_shift);
			state_reverse_compl = hn::Or(hn::ShiftRight<traits::bits_per_char>(state_reverse_compl), new_vec);
			return hn::Min(state_forward, state_reverse_compl);
#endif
		}

		// Returns per-lane mask where `true` means the current k-mer is palindromic
		// (state_forward == state_reverse_compl). Minimap2 / scalar skips palindromes
		// because "we don't know its strand". For SIMD parity the caller replaces
		// palindromic lanes' hash with max_val so they never become window minima —
		// matching the scalar's post-palindrome-push-max behaviour without requiring
		// per-lane pos tracking.
		[[nodiscard]] HWY_INLINE hn::Mask<d_t> get_palindrome_mask(const d_t& d) const {
#if HWY_TARGET_IS_SVE || HWY_TARGET == HWY_RVV
			return hn::Eq(
				hn::LoadU(d, state_forward_storage.data()),
				hn::LoadU(d, state_reverse_compl_storage.data()));
#else
			return hn::Eq(state_forward, state_reverse_compl);
#endif
		}

		// Returns per-lane mask where `true` means the reverse-complement state is the
		// minimum (so `min(forward, rc)` chose the rc — strand=1 / SEQUENCE_STRAND_REVERSE_COMPLEMENT)
		// and `false` means the forward state was the minimum (strand=0 / SEQUENCE_STRAND_FORWARD).
		// Mirrors `select_min_hash_strand`'s `forward < reverse_complement ? FORWARD : REVERSE_COMPLEMENT`
		// classification — palindromic lanes (forward == reverse_complement) report `false` here but
		// the caller separately masks them out via `get_palindrome_mask` and pushes a sentinel hash.
		[[nodiscard]] HWY_INLINE hn::Mask<d_t> get_strand_mask(const d_t& d) const {
#if HWY_TARGET_IS_SVE || HWY_TARGET == HWY_RVV
			return hn::Lt(
				hn::LoadU(d, state_reverse_compl_storage.data()),
				hn::LoadU(d, state_forward_storage.data()));
#else
			return hn::Lt(state_reverse_compl, state_forward);
#endif
		}


	};

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};
static_assert(::bioseq::simd::supports_scalar_hash<lexic_hasher, seq::dna4>);
static_assert(supports_simd_hash<lexic_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
