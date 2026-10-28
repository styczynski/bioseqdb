/**
 * @file bounded_nthash_seed_policy.cppm
 * @brief Native-rank rolling hash seeds shared by CPU and device algorithms.
 */
module;
#include <libassert/assert.hpp>

export module bioseq.sequences.kmers.bounded_nthash_seed_policy;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::minimizers {

/** Mix a stable biological alphabet identity and rank into one rolling seed. */
[[nodiscard]] constexpr std::uint32_t native_rank_seed(
    const std::uint32_t alphabet_code,
    const std::uint32_t rank) noexcept {
    std::uint32_t value = (0x9e3779b9u * (rank + 1u))
        ^ (0x85ebca6bu * (alphabet_code + 1u));
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

/**
 * Native rank semantics for rolling32 minimizers. Nucleotide hashes retain
 * canonical reverse complements; amino-acid hashes contain only forward state.
 * Every alphabet rank is a seedable symbol, including ambiguity and stop ranks.
 */
template<class p_alphabet>
struct bounded_nthash_seed_policy final {
    using alphabet_type = p_alphabet;
    inline static constexpr std::size_t alphabet_size =
        seqan3::alphabet_size<alphabet_type>;
    inline static constexpr std::size_t rank_bits =
        seqan3::detail::ceil_log2(alphabet_size);
    inline static constexpr bool canonical_reverse_complement =
        seqan3::nucleotide_alphabet<alphabet_type>;
    inline static constexpr std::uint32_t alphabet_code =
        static_cast<std::uint32_t>(
            supported_biological_alphabets::index_of<alphabet_type>());

    /** Test a decoded rank before indexing the seed array. */
    [[nodiscard]] static constexpr bool rank_is_seedable(
        const std::size_t rank) noexcept {
        return rank < alphabet_size;
    }

    /** Return the complement rank; this operation does not exist for proteins. */
    [[nodiscard]] static constexpr std::uint32_t complement_rank(
        const std::uint32_t rank) noexcept
        requires canonical_reverse_complement {
        alphabet_type symbol{};
        seqan3::assign_rank_to(rank, symbol);
        return static_cast<std::uint32_t>(seqan3::to_rank(seqan3::complement(symbol)));
    }

    /** Build the single seed table in the alphabet's rank order. */
    inline static constexpr auto forward_seeds = []() constexpr {
        std::array<std::uint32_t, alphabet_size> seeds{};
        for (std::size_t rank = 0u; rank < seeds.size(); ++rank) {
            if constexpr (std::same_as<alphabet_type, seq::dna4>
                          || std::same_as<alphabet_type, seq::dna5>) {
                alphabet_type symbol{};
                seqan3::assign_rank_to(rank, symbol);
                switch (seqan3::to_char(symbol)) {
                case 'A': seeds[rank] = 0x95c60474u; break;
                case 'C': seeds[rank] = 0x62a02b4cu; break;
                case 'G': seeds[rank] = 0x82572324u; break;
                case 'T': seeds[rank] = 0x4be24456u; break;
                default: seeds[rank] = 0xdfe2649cu; break;
                }
            } else {
                seeds[rank] = native_rank_seed(
                    alphabet_code, static_cast<std::uint32_t>(rank));
            }
        }
        return seeds;
    }();

    /** Scalar native rolling state; exact packed hashers intentionally have a different update API. */
    class scalar_state {
        std::size_t kmer_size_;
        std::uint32_t forward_ = 0u;
        [[no_unique_address]] std::array<std::uint32_t,
            canonical_reverse_complement ? 1u : 0u> reverse_{};

    public:
        /** Scalar contract reports the native scheme's runtime capacity, independent of rank width. */
        struct traits {
            static constexpr std::size_t k_max_kmer_length = 31u;
            using hasher_return_type = std::pair<std::uint32_t, sequence_strand_mode>;
        };

        /** Start a validated native32 k-mer without allocating or expanding source ranks. */
        explicit constexpr scalar_state(const std::size_t kmer_size) : kmer_size_(kmer_size) {
            if (kmer_size == 0u || kmer_size > traits::k_max_kmer_length) {
                DEBUG_ASSERT(false, "native rolling k-mer is outside its registered range");
                std::abort();
            }
        }

        /** Reset both available strand states after an invalid decoded rank. */
        constexpr void reset() noexcept {
            forward_ = 0u;
            if constexpr (canonical_reverse_complement) reverse_[0] = 0u;
        }

        /** Fold one initial rank at its zero-based position in the first complete k-mer. */
        constexpr void append_initial(const std::uint32_t rank, const std::size_t position) noexcept {
            DEBUG_ASSERT(rank_is_seedable(rank) && position < kmer_size_,
                "native rolling initialization requires an in-range rank and position");
            constexpr std::size_t width = std::numeric_limits<std::uint32_t>::digits;
            forward_ ^= std::rotl(forward_seeds[rank],
                static_cast<int>((7u * (kmer_size_ - 1u - position)) % width));
            if constexpr (canonical_reverse_complement) {
                reverse_[0] ^= std::rotl(forward_seeds[complement_rank(rank)],
                    static_cast<int>((7u * position) % width));
            }
        }

        /** Update the rolling state with the arriving and evicted native ranks. */
        constexpr void update(const std::uint32_t arriving, const std::uint32_t evicted) noexcept {
            DEBUG_ASSERT(rank_is_seedable(arriving) && rank_is_seedable(evicted),
                "native rolling update requires in-range ranks");
            constexpr std::size_t width = std::numeric_limits<std::uint32_t>::digits;
            forward_ = std::rotl(forward_, 7)
                ^ std::rotl(forward_seeds[evicted], static_cast<int>((7u * kmer_size_) % width))
                ^ forward_seeds[arriving];
            if constexpr (canonical_reverse_complement) {
                reverse_[0] = std::rotr(reverse_[0], 7)
                    ^ std::rotr(forward_seeds[complement_rank(evicted)], 7)
                    ^ std::rotl(forward_seeds[complement_rank(arriving)],
                        static_cast<int>((7u * (kmer_size_ - 1u)) % width));
            }
        }

        /** Return the common native32 hash and strand; equal DNA states choose forward. */
        [[nodiscard]] constexpr typename traits::hasher_return_type value() const noexcept {
            if constexpr (canonical_reverse_complement) {
                return {static_cast<std::uint32_t>(forward_ + reverse_[0]),
                    reverse_[0] < forward_ ? sequence_strand_mode::reverse_complement
                                          : sequence_strand_mode::forward};
            } else {
                return {forward_, sequence_strand_mode::forward};
            }
        }
    };
};

/** Require a biological alphabet with a byte-addressable rank and one seed per rank. */
template<class p_alphabet>
concept bounded_nthash_seeded_alphabet =
    (seqan3::nucleotide_alphabet<p_alphabet>
        || seqan3::aminoacid_alphabet<p_alphabet>)
    && (seqan3::alphabet_size<p_alphabet> >= 1u)
    && (seqan3::alphabet_size<p_alphabet> <= 256u)
    && requires { bounded_nthash_seed_policy<p_alphabet>::forward_seeds; }
    && (std::tuple_size_v<std::remove_cv_t<decltype(
            bounded_nthash_seed_policy<p_alphabet>::forward_seeds)>>
        == seqan3::alphabet_size<p_alphabet>);

static_assert(bounded_nthash_seeded_alphabet<seq::dna4>);
static_assert(bounded_nthash_seeded_alphabet<seq::dna5>);
static_assert(bounded_nthash_seeded_alphabet<seq::aa10li>);
static_assert(bounded_nthash_seeded_alphabet<seq::aa10murphy>);
static_assert(bounded_nthash_seeded_alphabet<seq::aa20>);
static_assert(bounded_nthash_seeded_alphabet<seq::aa27>);

} // namespace bioseq::minimizers
