/**
 * @file highway_lexic_kernel.cppm
 * @brief Fixed-target Highway lexicographic minimizer hasher.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_lexic;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

template<sequence_alphabet p_alphabet>
struct lexic_hasher : ::bioseq::simd::lexic_hasher<p_alphabet> {
public:
    /** Stateful SIMD canonical-kmer hash. */
    template<typename p_input_type, typename p_simd_tag>
    class simd_hash
        : public simd_hasher_base<p_alphabet, p_input_type, p_simd_tag> {
    public:
        using base_type =
            simd_hasher_base<p_alphabet, p_input_type, p_simd_tag>;
        using traits = typename base_type::traits;

    private:
        typename traits::input_vector_type mask_;
        typename traits::input_vector_type forward_state_;
        typename traits::input_vector_type reverse_complement_state_;
        std::size_t reverse_shift_;

		// Compile-time complement table: index = rank, value = complement(rank).
		// We cannot use `Xor(rank, char_mask)` in general — that works for dna4
		// because its complement pairs are {0,3} and {1,2} (xor 3 swaps them),
		// but for dna5 the pairs are {0 A ↔ 4 T}, {1 C ↔ 2 G}, {3 N self} and no
		// single XOR mask covers all three cases. A static lookup table does.
        static constexpr auto make_complement_lut() {
            std::array<p_input_type, traits::alphabet_size> lut{};
            for (std::size_t rank = 0; rank < traits::alphabet_size; ++rank) {
                lut[rank] = static_cast<p_input_type>(traits::letter_complement(
                    static_cast<typename traits::storage_unit>(rank)));
            }
            return lut;
        }

        static constexpr std::array<p_input_type, traits::alphabet_size>
            k_complement_lut = make_complement_lut();

		// Detect XOR-complement alphabets at compile time. dna4 is the
		// production case: complement pairs are {0,3} and {1,2}, both of
		// which are flipped by XOR with the value `alphabet_size - 1 = 3`.
		// One vector XOR is dramatically cheaper than the (alphabet_size-1)
		// chained IfThenElse cascade the LUT path falls back to — for dna4
		// this is one of the dominant per-iteration costs of the SIMD sketch.
		// dna5 fails this predicate ({0,4} flips ok, {1,2} flips ok, {3,3}
		// — N — must stay at 3 but `3 ^ 4 = 7`) so the LUT remains the
		// fallback for non-power-of-2 alphabets.
		static constexpr bool k_is_xor_complement = []() constexpr {
            using storage_unit = typename traits::storage_unit;
            constexpr storage_unit k_xor_mask =
                static_cast<storage_unit>(traits::alphabet_size - 1);
            for (std::size_t rank = 0; rank < traits::alphabet_size; ++rank) {
                const storage_unit expected =
                    static_cast<storage_unit>(rank) ^ k_xor_mask;
                const storage_unit actual = static_cast<storage_unit>(
                    traits::letter_complement(
                        static_cast<storage_unit>(rank)));
                if (expected != actual) return false;
            }
            return true;
        }();

		// Compute `complement(outgoing)` per lane. For dna4 a single Xor with
		// (alphabet_size - 1) suffices; for non-XOR alphabets we fall back to
		// the IfThenElse cascade over the compile-time LUT. The branch is
		// `if constexpr` so each instantiation gets exactly one code path.
        [[always_inline]] static constexpr typename traits::input_vector_type
        apply_complement(
            const typename traits::input_simd_tag_type& simd_tag,
            typename traits::input_vector_type rank) {
            if constexpr (k_is_xor_complement) {
                return hn::Xor(
                    rank,
                    hn::Set(
                        simd_tag,
                        static_cast<p_input_type>(traits::alphabet_size - 1)));
            } else {
                typename traits::input_vector_type result = hn::Set(
                    simd_tag, k_complement_lut[traits::alphabet_size - 1]);
                for (int table_rank =
                         static_cast<int>(traits::alphabet_size) - 2;
                     table_rank >= 0;
                     --table_rank) {
                    result = hn::IfThenElse(
                        hn::Eq(
                            rank,
                            hn::Set(
                                simd_tag,
                                static_cast<p_input_type>(table_rank))),
                        hn::Set(simd_tag, k_complement_lut[table_rank]),
                        result);
                }
                return result;
            }
        }

    public:
        /** Initialize every rolling lane for the requested k-mer length. */
        explicit simd_hash(
            const typename traits::input_simd_tag_type& simd_tag,
            std::size_t kmer_length)
            : mask_(hn::Set(
                  simd_tag,
                  traits::template kmer_mask<p_input_type>(
                      std::min(kmer_length, traits::k_max_kmer_length)))),
              forward_state_(hn::Zero(simd_tag)),
              reverse_complement_state_(hn::Zero(simd_tag)),
              reverse_shift_(traits::bits_per_char
                  * (std::min(kmer_length, traits::k_max_kmer_length) - 1u)) {}

        /** Consume one vector of incoming and outgoing alphabet codes. */
        HWY_INLINE typename traits::hasher_return_type next(
            const typename traits::input_simd_tag_type& simd_tag,
            typename traits::input_vector_type incoming,
            typename traits::input_vector_type outgoing) {
            forward_state_ = hn::And(
                hn::Or(
                    hn::ShiftLeft<traits::bits_per_char>(forward_state_),
                    incoming),
                mask_);
            const typename traits::input_vector_type complement =
                hn::ShiftLeftSame(
                    apply_complement(simd_tag, outgoing), reverse_shift_);
            reverse_complement_state_ = hn::Or(
                hn::ShiftRight<traits::bits_per_char>(
                    reverse_complement_state_),
                complement);
            return hn::Min(forward_state_, reverse_complement_state_);
        }

		// Returns per-lane mask where `true` means the current k-mer is palindromic
		// (state_forward == state_reverse_compl). Minimap2 / scalar skips palindromes
		// because "we don't know its strand". For SIMD parity the caller replaces
		// palindromic lanes' hash with max_val so they never become window minima —
		// matching the scalar's post-palindrome-push-max behaviour without requiring
		// per-lane pos tracking.
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> palindrome_mask(
            const p_simd_tag&) const {
            return hn::Eq(forward_state_, reverse_complement_state_);
        }

		// Returns per-lane mask where `true` means the reverse-complement state is the
		// minimum (so `min(forward, rc)` chose reverse-complement strand 1)
		// and `false` means the forward state was the minimum (strand 0).
		// Mirrors `select_min_hash_strand`'s `forward < reverse_complement ? FORWARD : REVERSE_COMPLEMENT`
		// classification — palindromic lanes (forward == reverse_complement) report `false` here but
		// the caller separately masks them out via `get_palindrome_mask` and pushes a sentinel hash.
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> reverse_strand_mask(
            const p_simd_tag&) const {
            return hn::Lt(reverse_complement_state_, forward_state_);
        }
    };
};
static_assert(::bioseq::simd::supports_scalar_hash<lexic_hasher, seq::dna4>);
static_assert(supports_simd_hash<lexic_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
