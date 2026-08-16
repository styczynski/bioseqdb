/**
 * @file highway_lexic_kernel.cppm
 * @brief Fixed-target Highway lexicographic minimizer hasher.
 *
 * @details Maintains forward and reverse-complement codes independently in every lane, exposes
 * palindrome and strand masks, and leaves window ordering to the minimizer kernel.
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

/**
 * @brief Adds a fixed-target rolling lexicographic hash to the scalar hasher policy.
 * @tparam p_alphabet Sequence alphabet whose rank and complement rules define the rolling state.
 */
template <sequence_alphabet p_alphabet>
struct lexic_hasher : ::bioseq::simd::lexic_hasher<p_alphabet> {
  public:
    /**
     * @brief Maintains canonical k-mer state independently in each SIMD lane.
     * @tparam p_input_type Unsigned lane type holding a packed k-mer code.
     * @tparam p_simd_tag Highway descriptor for the generated target.
     */
    template <typename p_input_type, typename p_simd_tag>
    class simd_hash : public simd_hasher_base<p_alphabet, p_input_type, p_simd_tag> {
      public:
        /** @brief Fixed-target SIMD base supplying vector traits. */
        using base_type = simd_hasher_base<p_alphabet, p_input_type, p_simd_tag>;
        /** @brief Lane, mask, and alphabet limits for this specialization. */
        using traits = typename base_type::traits;

      private:
        typename traits::input_vector_type mask_;
        typename traits::input_vector_type forward_state_;
        typename traits::input_vector_type reverse_complement_state_;
        std::size_t reverse_shift_;

        /**
         * @brief Builds the rank-indexed complement table for non-XOR alphabets.
         * @return Array mapping every alphabet rank to its complement rank.
         */
        static constexpr auto make_complement_lut() {
            std::array<p_input_type, traits::alphabet_size> lut{};
            for (std::size_t rank = 0; rank < traits::alphabet_size; ++rank) {
                lut[rank] = static_cast<p_input_type>(
                    traits::letter_complement(static_cast<typename traits::storage_unit>(rank)));
            }
            return lut;
        }

        /** @brief Compile-time rank-to-complement mapping for the selected alphabet. */
        static constexpr std::array<p_input_type, traits::alphabet_size> k_complement_lut =
            make_complement_lut();

        /**
         * @brief Indicates whether complementing every rank is one XOR by `alphabet_size - 1`.
         *
         * XOR-capable alphabets use one vector operation. Other alphabets use the compile-time
         * lookup cascade so self-complementing and non-power-of-two rank layouts remain exact.
         */
        static constexpr bool k_is_xor_complement = []() constexpr {
            using storage_unit = typename traits::storage_unit;
            constexpr storage_unit k_xor_mask =
                static_cast<storage_unit>(traits::alphabet_size - 1);
            for (std::size_t rank = 0; rank < traits::alphabet_size; ++rank) {
                const storage_unit expected = static_cast<storage_unit>(rank) ^ k_xor_mask;
                const storage_unit actual = static_cast<storage_unit>(
                    traits::letter_complement(static_cast<storage_unit>(rank)));
                if (expected != actual)
                    return false;
            }
            return true;
        }();

        /**
         * @brief Computes the complement rank in every active lane.
         * @param[in] simd_tag Highway descriptor selecting the fixed-target operations.
         * @param[in] rank Per-lane alphabet ranks.
         * @return Per-lane complements produced by the XOR or lookup specialization.
         */
        [[always_inline]] static constexpr typename traits::input_vector_type
        apply_complement(const typename traits::input_simd_tag_type& simd_tag,
                         typename traits::input_vector_type rank) {
            if constexpr (k_is_xor_complement) {
                return hn::Xor(
                    rank, hn::Set(simd_tag, static_cast<p_input_type>(traits::alphabet_size - 1)));
            } else {
                typename traits::input_vector_type result =
                    hn::Set(simd_tag, k_complement_lut[traits::alphabet_size - 1]);
                for (int table_rank = static_cast<int>(traits::alphabet_size) - 2; table_rank >= 0;
                     --table_rank) {
                    result = hn::IfThenElse(
                        hn::Eq(rank, hn::Set(simd_tag, static_cast<p_input_type>(table_rank))),
                        hn::Set(simd_tag, k_complement_lut[table_rank]),
                        result);
                }
                return result;
            }
        }

      public:
        /**
         * @brief Initializes every rolling lane for the requested k-mer length.
         * @param[in] simd_tag Highway descriptor selecting the fixed-target operations.
         * @param[in] kmer_length Positive number of symbols in each rolling k-mer, clamped to the
         * lane capacity.
         */
        explicit simd_hash(const typename traits::input_simd_tag_type& simd_tag,
                           std::size_t kmer_length)
            : mask_(hn::Set(simd_tag,
                            traits::template kmer_mask<p_input_type>(
                                std::min(kmer_length, traits::k_max_kmer_length)))),
              forward_state_(hn::Zero(simd_tag)), reverse_complement_state_(hn::Zero(simd_tag)),
              reverse_shift_(traits::bits_per_char *
                             (std::min(kmer_length, traits::k_max_kmer_length) - 1u)) {}

        /**
         * @brief Advances the forward and reverse-complement rolling states by one symbol.
         * @param[in] simd_tag Highway descriptor selecting the fixed-target operations.
         * @param[in] incoming Alphabet rank entering the right edge of each lane's k-mer.
         * @param[in] outgoing Alphabet rank leaving the left edge of each lane's k-mer.
         * @return Per-lane minimum of the forward and reverse-complement packed codes.
         */
        HWY_INLINE typename traits::hasher_return_type
        next(const typename traits::input_simd_tag_type& simd_tag,
             typename traits::input_vector_type incoming,
             typename traits::input_vector_type outgoing) {
            forward_state_ = hn::And(
                hn::Or(hn::ShiftLeft<traits::bits_per_char>(forward_state_), incoming), mask_);
            const typename traits::input_vector_type complement =
                hn::ShiftLeftSame(apply_complement(simd_tag, outgoing), reverse_shift_);
            reverse_complement_state_ = hn::Or(
                hn::ShiftRight<traits::bits_per_char>(reverse_complement_state_), complement);
            return hn::Min(forward_state_, reverse_complement_state_);
        }

        /**
         * @brief Identifies lanes whose forward and reverse-complement codes are equal.
         * @return Per-lane palindrome mask. Callers replace selected lanes with the maximum hash so
         * they cannot become window minima.
         */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> palindrome_mask(const p_simd_tag&) const {
            return hn::Eq(forward_state_, reverse_complement_state_);
        }

        /**
         * @brief Identifies lanes whose reverse-complement code is strictly smaller.
         * @return Per-lane reverse-strand mask. Equal codes report false and are independently
         * identified by `palindrome_mask()`.
         */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag> reverse_strand_mask(const p_simd_tag&) const {
            return hn::Lt(reverse_complement_state_, forward_state_);
        }
    };
};
static_assert(::bioseq::simd::supports_scalar_hash<lexic_hasher, seq::dna4>);
static_assert(supports_simd_hash<lexic_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
