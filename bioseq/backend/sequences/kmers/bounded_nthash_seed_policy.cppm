/**
 * @file bounded_nthash_seed_policy.cppm
 * @brief Supplies one ntHash seed for each supported alphabet symbol.
 */
module;

export module bioseq.sequences.kmers.bounded_nthash_seed_policy;

import std;
import bioseq.common.seqan_adapter;

export namespace bioseq::minimizers {

/**
 * @brief Defines ntHash seeds in alphabet rank order.
 *
 * Add a specialization to supply another nucleotide alphabet's seeds, then
 * register that alphabet with the kernel generator. The SIMD lookup uses
 * these tables and the alphabet's complement operation.
 */
template<class p_alphabet>
struct bounded_nthash_seed_policy;

/** SeqAn DNA4 rank order: A, C, G, T. */
template<>
struct bounded_nthash_seed_policy<seq::dna4> final {
    inline static constexpr std::array<std::uint32_t, 4> forward_seeds{
        0x95c60474u,
        0x62a02b4cu,
        0x82572324u,
        0x4be24456u,
    };
};

/** SeqAn DNA5 rank order: A, C, G, N, T. */
template<>
struct bounded_nthash_seed_policy<seq::dna5> final {
    inline static constexpr std::array<std::uint32_t, 5> forward_seeds{
        bounded_nthash_seed_policy<seq::dna4>::forward_seeds[0],
        bounded_nthash_seed_policy<seq::dna4>::forward_seeds[1],
        bounded_nthash_seed_policy<seq::dna4>::forward_seeds[2],
        0xdfe2649cu,
        bounded_nthash_seed_policy<seq::dna4>::forward_seeds[3],
    };
};

/** Requires a nucleotide alphabet of at most 256 symbols with one seed per rank. */
template<class p_alphabet>
concept bounded_nthash_seeded_alphabet =
    seqan3::nucleotide_alphabet<p_alphabet>
    && (seqan3::alphabet_size<p_alphabet> >= 1u)
    && (seqan3::alphabet_size<p_alphabet> <= 256u)
    && requires {
        bounded_nthash_seed_policy<p_alphabet>::forward_seeds;
    }
    && (std::tuple_size_v<std::remove_cv_t<decltype(
            bounded_nthash_seed_policy<p_alphabet>::forward_seeds)>>
        == seqan3::alphabet_size<p_alphabet>);

static_assert(bounded_nthash_seeded_alphabet<seq::dna4>);
static_assert(bounded_nthash_seeded_alphabet<seq::dna5>);

} // namespace bioseq::minimizers
