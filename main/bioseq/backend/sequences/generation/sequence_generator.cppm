/**
 * @file sequence_generator.cppm
 * @brief Synthetic sequence generator, modelled after SEAL.
 *
 * DOC0264 §10.1 — moved from `main/bench/sequence_generator.cppm` to the
 * bioseq backend so the `bioseq.create_sample_table` SQL UDF can call it
 * directly. The namespace is renamed from `bioseq::bench::HWY_NAMESPACE`
 * to `bioseq::sequences::generation::HWY_NAMESPACE`; a `bioseq::bench`
 * alias re-exports the symbols so the bench framework's existing imports
 * keep working unchanged. DOC0286 §10.0a makes the generator generic over
 * <alphabet, storage_unit>; `generate_single_sequence<A,U>(bp_size, seed)`
 * is the per-sequence helper for the UDF's per-sequence emission.
 *
 * Produces (query, target) pairs with ground-truth position metadata so
 * downstream evaluation can classify each aligner hit as correct or
 * incorrect. The error model matches SEAL's core generators — uniform
 * substitutions, normally-distributed indel lengths, optional
 * reverse-complement flipping — but we skip SEAL's SAM-header round-trip
 * because our evaluator consumes ground truth directly from this struct.
 *
 * Interface:
 *   GeneratorParams                — simulation knobs (reference length, error rates, ...)
 *   GeneratedBenchSequencePair     — single (query, target) with ground truth
 *   generate_test_sequences<A,U>() — fresh batch, seeded by params.seed
 *   generate_single_sequence<A,U>() — DOC0264 §10.1 / DOC0286 §10.0a: per-sequence helper for the UDF
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

export module bioseq_sequences.generation;

import mlogging;
import sequan_compat;
import bioseq_sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::sequences::generation::HWY_NAMESPACE {

/// Simulation knobs passed to `generate_test_sequences()`.
///
/// Substitution + indel models mirror SEAL's UniformErrorGenerator and
/// IndelGenerator. Lengths are sampled from Gaussians; rates are per-base
/// probabilities. Deletion lengths are clamped to >= 0 (SEAL has a bug
/// where negative samples rewind position; we don't reproduce it).
struct GeneratorParams {
    size_t reference_length      = 10'000;
    size_t fragment_count        = 100;
    size_t fragment_length_mean  = 150;
    double fragment_length_stddev = 10.0;

    double substitution_rate     = 0.01;
    double insertion_rate        = 0.0005;
    double deletion_rate         = 0.0005;

    double insertion_length_mean   = 2.0;
    double insertion_length_stddev = 1.0;
    double deletion_length_mean    = 2.0;
    double deletion_length_stddev  = 1.0;

    double   reverse_complement_rate = 0.5;
    uint64_t seed                    = 42;
};

/// One generated (query, target) pair with ground-truth metadata. `query`
/// is the fragment extracted from `target` at [truth_target_start,
/// truth_target_end), optionally reverse-complemented and with errors
/// injected. `target` is the same synthetic reference shared by all pairs
/// in a batch — duplicated per pair so the struct can be handed to a
/// single-pair evaluator without tracking shared storage separately.
template <sequence_alphabet alphabet_t, typename storage_unit_t = std::uint32_t>
struct GeneratedBenchSequencePair {
    using config_t   = sequence_custom_alphabet<alphabet_t, storage_unit_t>;
    using sequence_t = compact_sequence_range<config_t>;
    sequence_t  query;
    sequence_t  target;

    size_t truth_target_start = 0;
    size_t truth_target_end   = 0;
    bool   is_reverse         = false;

    double effective_error_rate  = 0.0;
    size_t original_query_length = 0;
};

namespace detail {

[[nodiscard]] constexpr size_t clamp_gaussian_to_nonneg_bp(double raw_length) {
    return raw_length > 0.0 ? static_cast<size_t>(raw_length) : size_t{0};
}

// DOC0286 §10.0a — these helpers are templated over `alphabet_t`. The reference
// is drawn uniformly from the four CANONICAL nucleotides A/C/G/T (via `assign_char`,
// which is rank-correct for dna4 AND dna5), then — for an alphabet that carries an
// ambiguity code (dna5: `alphabet_size > 4`) — SPARSE N is sprinkled in at ~1%.
//
// Why not the naive `base_dist(0, alphabet_size - 1)`? For dna5 that draws N at a
// uniform 20%, which leaves essentially NO N-free k-mer anywhere in the reference,
// so NO minimizer-based mapper (bioseq OR minimap2) can seed it — every E2E query
// would fail to map and the benchmark/smoke would prove nothing. Real genomic N
// marks rare assembly gaps; ~1% sparse N exercises the dna5 codec + the minimizer
// N-reset while leaving long N-free runs for the seeds.
//
// dna4 is BYTE-IDENTICAL to the old `base_dist(0, 3)` draws: `kBases[j]` assigns to
// rank `j` for dna4 (A=0,C=1,G=2,T=3), and exactly one RNG draw is consumed per
// position, same as before. dna5's extra N-injection draws only affect dna5.
template <sequence_alphabet alphabet_t>
[[nodiscard]] inline std::vector<alphabet_t> build_random_reference(
    size_t length, std::mt19937_64& rng)
{
    static constexpr char kBases[4] = {'A', 'C', 'G', 'T'};
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::vector<alphabet_t> reference(length);
    for (size_t position = 0; position < length; ++position) {
        reference[position].assign_char(kBases[base_dist(rng)]);
    }
    if constexpr (seqan3::alphabet_size<alphabet_t> > 4) {
        std::uniform_int_distribution<int> n_dist(0, 99);
        for (size_t position = 0; position < length; ++position) {
            if (n_dist(rng) < 1) reference[position].assign_char('N');  // ~1% N
        }
    }
    return reference;
}

// Reverse-complement is nucleotide-only (`seqan3::complement` is defined only
// for nucleotide alphabets). The `if constexpr` guard keeps this template
// instantiable for any alphabet; non-nucleotide alphabets are merely reversed.
template <sequence_alphabet alphabet_t>
inline void reverse_complement_in_place(std::vector<alphabet_t>& sequence) {
    std::reverse(sequence.begin(), sequence.end());
    if constexpr (seqan3::nucleotide_alphabet<alphabet_t>) {
        for (auto& letter : sequence) {
            letter = seqan3::complement(letter);
        }
    }
}

template <sequence_alphabet alphabet_t>
inline size_t apply_uniform_substitutions(
    std::vector<alphabet_t>& sequence, double rate, std::mt19937_64& rng)
{
    if (rate <= 0.0) return 0;
    std::uniform_real_distribution<double> probability(0.0, 1.0);
    std::uniform_int_distribution<int> base_dist(
        0, static_cast<int>(seqan3::alphabet_size<alphabet_t>) - 1);
    size_t substitution_count = 0;
    for (auto& letter : sequence) {
        if (probability(rng) < rate) {
            const int original_rank = static_cast<int>(letter.to_rank());
            int replacement_rank = base_dist(rng);
            while (replacement_rank == original_rank) {
                replacement_rank = base_dist(rng);
            }
            letter.assign_rank(
                static_cast<seqan3::alphabet_rank_t<alphabet_t>>(replacement_rank));
            ++substitution_count;
        }
    }
    return substitution_count;
}

template <sequence_alphabet alphabet_t>
[[nodiscard]] inline std::vector<alphabet_t> apply_indels(
    const std::vector<alphabet_t>& sequence,
    double insertion_rate, double insertion_mean, double insertion_stddev,
    double deletion_rate,  double deletion_mean,  double deletion_stddev,
    std::mt19937_64& rng)
{
    std::vector<alphabet_t> mutated_sequence;
    mutated_sequence.reserve(sequence.size() + sequence.size() / 8);
    std::uniform_real_distribution<double> probability(0.0, 1.0);
    std::uniform_int_distribution<int> base_dist(
        0, static_cast<int>(seqan3::alphabet_size<alphabet_t>) - 1);
    std::normal_distribution<double> insertion_length(insertion_mean, insertion_stddev);
    std::normal_distribution<double> deletion_length(deletion_mean, deletion_stddev);

    size_t position = 0;
    while (position < sequence.size()) {
        mutated_sequence.push_back(sequence[position]);
        if (insertion_rate > 0.0 && probability(rng) < insertion_rate) {
            const size_t insertion_event_length =
                clamp_gaussian_to_nonneg_bp(std::round(insertion_length(rng)));
            for (size_t inserted_position = 0;
                 inserted_position < insertion_event_length;
                 ++inserted_position)
            {
                alphabet_t inserted_letter;
                inserted_letter.assign_rank(
                    static_cast<seqan3::alphabet_rank_t<alphabet_t>>(base_dist(rng)));
                mutated_sequence.push_back(inserted_letter);
            }
        }
        size_t advance_step = 1;
        if (deletion_rate > 0.0 && probability(rng) < deletion_rate) {
            const size_t deletion_event_length =
                clamp_gaussian_to_nonneg_bp(std::round(deletion_length(rng)));
            advance_step += deletion_event_length;
        }
        position += advance_step;
    }
    return mutated_sequence;
}

[[nodiscard]] inline size_t sample_fragment_length(
    size_t mean, double stddev, size_t upper_bound, std::mt19937_64& rng)
{
    if (upper_bound <= 1) return 1;
    std::normal_distribution<double> length_dist(
        static_cast<double>(mean), stddev);
    size_t length = clamp_gaussian_to_nonneg_bp(std::round(length_dist(rng)));
    if (length == 0) length = 1;
    if (length >= upper_bound) length = upper_bound - 1;
    return length;
}

}  // namespace detail

/// Generate a fresh batch of (query, target) pairs. Builds one synthetic
/// reference, then samples fragments from it, optionally reverse-
/// complements, injects SEAL-style substitution + indel errors, and
/// records the ground-truth origin. Deterministic for a given params.seed.
template <sequence_alphabet alphabet_t, typename storage_unit_t = std::uint32_t>
[[nodiscard]] inline std::vector<GeneratedBenchSequencePair<alphabet_t, storage_unit_t>>
generate_test_sequences(const GeneratorParams& params) {
    using pair_t  = GeneratedBenchSequencePair<alphabet_t, storage_unit_t>;
    using range_t = typename pair_t::sequence_t;
    std::mt19937_64 rng(params.seed);

    std::vector<alphabet_t> reference =
        detail::build_random_reference<alphabet_t>(params.reference_length, rng);

    std::vector<pair_t> pairs;
    pairs.reserve(params.fragment_count);

    std::uniform_real_distribution<double> probability(0.0, 1.0);

    for (size_t fragment_index = 0; fragment_index < params.fragment_count; ++fragment_index) {
        const size_t fragment_length = detail::sample_fragment_length(
            params.fragment_length_mean, params.fragment_length_stddev,
            params.reference_length, rng);

        std::uniform_int_distribution<size_t> start_dist(
            0, params.reference_length - fragment_length);
        const size_t fragment_start = start_dist(rng);
        const size_t fragment_end   = fragment_start + fragment_length;

        std::vector<alphabet_t> fragment(
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_start),
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_end));

        const bool is_reverse =
            probability(rng) < params.reverse_complement_rate;
        if (is_reverse) {
            detail::reverse_complement_in_place<alphabet_t>(fragment);
        }

        detail::apply_uniform_substitutions<alphabet_t>(fragment, params.substitution_rate, rng);
        fragment = detail::apply_indels<alphabet_t>(
            fragment,
            params.insertion_rate, params.insertion_length_mean, params.insertion_length_stddev,
            params.deletion_rate,  params.deletion_length_mean,  params.deletion_length_stddev,
            rng);

        pair_t pair;
        pair.query = range_t(std::move(fragment));
        pair.target = range_t(reference);
        pair.truth_target_start = fragment_start;
        pair.truth_target_end   = fragment_end;
        pair.is_reverse         = is_reverse;
        pair.effective_error_rate =
            params.substitution_rate + params.insertion_rate + params.deletion_rate;
        pair.original_query_length = fragment_length;
        pairs.push_back(std::move(pair));
    }

    bioseq_log<DBG>("[bioseq.sequences.generation] Generated {} fragment pairs over a {} bp reference "
            "(sub_rate={}, ins_rate={}, del_rate={})",
            pairs.size(), params.reference_length,
            params.substitution_rate, params.insertion_rate, params.deletion_rate);
    return pairs;
}

/// DOC0264 §10.1 / DOC0286 §10.0a — generate a single uniform-random
/// sequence of `bp_size` letters using a stream-stateful std::mt19937_64
/// (one RNG state advanced across the whole sequence — NOT reseeded per
/// fragment; consolidated-review I39). Generic over the alphabet; the
/// `storage_unit_t` parameter is kept for signature symmetry with the rest
/// of the generator (the returned vector is unpacked `alphabet_t`). Used by
/// the `bioseq.create_sample_table` UDF's per-sequence producer.
template <sequence_alphabet alphabet_t, typename storage_unit_t = std::uint32_t>
[[nodiscard]] inline std::vector<alphabet_t>
generate_single_sequence(std::size_t bp_size, std::uint64_t seed)
{
    std::mt19937_64 rng{seed};
    std::uniform_int_distribution<int> base_dist(
        0, static_cast<int>(seqan3::alphabet_size<alphabet_t>) - 1);
    std::vector<alphabet_t> out(bp_size);
    for (std::size_t position = 0; position < bp_size; ++position) {
        out[position].assign_rank(
            static_cast<seqan3::alphabet_rank_t<alphabet_t>>(base_dist(rng)));
    }
    return out;
}

}  // namespace bioseq::sequences::generation::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

// DOC0264 §10.1 — namespace alias for source-compat with the legacy
// bench import. After Highway dispatches into N_NEON (or whatever target),
// the alias re-exports those symbols under the old `bioseq::bench` name.
namespace bioseq::bench {
    using namespace bioseq::sequences::generation;
}

}  // export
