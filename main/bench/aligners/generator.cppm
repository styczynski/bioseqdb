/**
 * @file generator.cppm
 * @brief Synthetic benchmark sequence generator, modelled after SEAL.
 *
 * Produces `(query, target)` pairs with ground-truth position metadata so
 * downstream evaluation can classify each aligner hit as correct or
 * incorrect. The error model matches SEAL's core generators — uniform
 * substitutions, normally-distributed indel lengths, optional
 * reverse-complement flipping — but we skip SEAL's SAM-header round-trip
 * because our evaluator consumes ground truth directly from this struct.
 *
 * Interface:
 *   GeneratorParams   — simulation knobs (reference length, error rates, ...)
 *   GeneratedBenchSequencePair — single (query, target) with ground truth
 *   generate_test_sequences()  — fresh batch, seeded by params.seed
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

export module bench.aligners.generator;

import mlogging;
import sequan_compat;
import bioseq_sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

/// Alphabet config used by the benchmark pipeline.
///
/// Originally `sequence_custom_alphabet<seq::dna4, uint64_t>` (32 letters
/// per uint64_t). Switched to `uint32_t` storage (16 letters per uint32_t):
///
/// 1. `unit_leftover_bits == 0` for both — bit-packed encoding stays the
///    same, V3 unit-aligned `to_encoded_into` fast path stays in scope.
/// 2. `hn::ScalableTag<uint32_t>` gives 4 lanes per 128-bit vector instead
///    of 2 for uint64_t. The `mm_sketch_simd` rolling-window kernel runs
///    4× independent streams instead of 2×, halving the iteration count.
/// 3. `uint32_t` is wide enough to hold the 30-bit k-mer hash for k ≤ 15
///    (default minimap2 `map-ont` preset), so the templated hash inside
///    `simd_hash<storage_unit, dd_t>` stays correct without overrides.
///    For larger k a uint64_t hash override would be needed; not in scope
///    for the long-sequence bench.
///
/// Memory footprint per 1 Mbp sequence is unchanged (250 kB in either
/// layout — 8 bits per pair stored, just at different word granularity).
using bench_alphabet_config = sequence_custom_alphabet<seq::dna4, uint32_t>;
using bench_sequence_range  = compact_sequence_range<bench_alphabet_config>;

/**
 * @brief Simulation knobs passed to `generate_test_sequences()`.
 *
 * Substitution + indel models mirror SEAL's UniformErrorGenerator and
 * IndelGenerator (generator/errors/*.java). Lengths are sampled from
 * Gaussians just like SEAL does; rates are per-base probabilities.
 *
 * Two intentional divergences from SEAL we have already accepted:
 *   * Error-generator ordering is hardcoded (substitutions first, then
 *     indels). SEAL applies a caller-supplied list of generators in
 *     order (Fragmentizer.java:168-171); since its default suite runs
 *     substitutions before indels we chose this fixed order rather
 *     than shipping a generator-plugin API.
 *   * Deletion lengths are clamped to >= 0. SEAL's IndelGenerator
 *     (IndelGenerator.java:102-121) uses `i += deleteLength` without a
 *     floor, so a negative Gaussian sample rewinds `i` and can emit a
 *     duplicated character. We consider the SEAL behaviour a bug and
 *     clamp, matching SEAL's own insertion-length clamp.
 */
struct GeneratorParams {
    /// Length (bp) of the synthetic reference shared by all fragments.
    size_t reference_length = 10'000;

    /// Number of fragments (queries) to produce.
    size_t fragment_count = 100;

    /// Gaussian mean / stddev for fragment length (bp).
    size_t fragment_length_mean = 150;
    double fragment_length_stddev = 10.0;

    /// Per-base probability of a substitution error. Mirrors SEAL's
    /// UniformErrorGenerator with a constant rate (no positional bias).
    double substitution_rate = 0.01;

    /// Per-base probability of starting an insertion / deletion event.
    double insertion_rate = 0.0005;
    double deletion_rate  = 0.0005;

    /// Gaussian mean / stddev for indel event lengths (bp).
    double insertion_length_mean   = 2.0;
    double insertion_length_stddev = 1.0;
    double deletion_length_mean    = 2.0;
    double deletion_length_stddev  = 1.0;

    /// Probability that a generated fragment is taken from the reverse
    /// strand of the reference (reverse-complemented before errors are
    /// applied).
    double reverse_complement_rate = 0.5;

    /// Deterministic RNG seed. Same seed -> identical batch output.
    uint64_t seed = 42;
};

/**
 * @brief One generated (query, target) pair with ground-truth metadata.
 *
 * `query` is the fragment extracted from `target` at
 * [truth_target_start, truth_target_end), optionally reverse-
 * complemented, with errors injected. `target` is the same synthetic
 * reference shared by all pairs in a batch — duplicated per pair so the
 * struct can be handed to single-pair evaluation harnesses without
 * tracking shared storage separately.
 */
struct GeneratedBenchSequencePair {
    bench_sequence_range query;
    bench_sequence_range target;

    size_t truth_target_start = 0;
    size_t truth_target_end   = 0;
    bool   is_reverse         = false;

    /// Effective per-base error rate (substitutions + insertions + deletions).
    double effective_error_rate = 0.0;

    /// Pre-error query length, used by the evaluator to rescale
    /// position_offset into a fractional error.
    size_t original_query_length = 0;
};

namespace detail {

/// Round a (possibly negative) Gaussian sample down to a non-negative
/// bp count. SEAL's IndelGenerator clamps insertion lengths this way
/// but not deletions — we apply it uniformly so a bad RNG draw never
/// produces backward progress in the indel loop. Kept as a named
/// helper so the divergence from SEAL is easy to find later.
[[nodiscard]] constexpr size_t clamp_gaussian_to_nonneg_bp(double raw_length) {
    return raw_length > 0.0 ? static_cast<size_t>(raw_length) : size_t{0};
}

/// Generate a uniform-random dna4 sequence of the requested length.
[[nodiscard]] inline std::vector<seq::dna4> build_random_reference(
    size_t length, std::mt19937_64& rng)
{
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::vector<seq::dna4> reference(length);
    for (size_t position = 0; position < length; ++position) {
        reference[position].assign_rank(
            static_cast<seqan3::alphabet_rank_t<seq::dna4>>(base_dist(rng)));
    }
    return reference;
}

/// Reverse-complement a dna4 sequence in place.
inline void reverse_complement_in_place(std::vector<seq::dna4>& sequence) {
    std::reverse(sequence.begin(), sequence.end());
    for (auto& letter : sequence) {
        letter = seqan3::complement(letter);
    }
}

/// Apply SEAL-style uniform substitution errors to a sequence. Each base
/// is independently replaced with probability `rate`; the replacement is
/// chosen uniformly from the other three bases so a substitution is
/// always visible (matches SEAL's SubstitutionErrorGenerator).
inline size_t apply_uniform_substitutions(
    std::vector<seq::dna4>& sequence, double rate, std::mt19937_64& rng)
{
    if (rate <= 0.0) return 0;
    std::uniform_real_distribution<double> probability(0.0, 1.0);
    std::uniform_int_distribution<int> base_dist(0, 3);
    size_t substitution_count = 0;
    for (auto& letter : sequence) {
        if (probability(rng) < rate) {
            const int original_rank = static_cast<int>(letter.to_rank());
            int replacement_rank = base_dist(rng);
            while (replacement_rank == original_rank) {
                replacement_rank = base_dist(rng);
            }
            letter.assign_rank(
                static_cast<seqan3::alphabet_rank_t<seq::dna4>>(replacement_rank));
            ++substitution_count;
        }
    }
    return substitution_count;
}

/// Apply SEAL-style independent insertion / deletion events to a sequence.
/// Returns the resulting sequence (possibly with different length).
[[nodiscard]] inline std::vector<seq::dna4> apply_indels(
    const std::vector<seq::dna4>& sequence,
    double insertion_rate, double insertion_mean, double insertion_stddev,
    double deletion_rate,  double deletion_mean,  double deletion_stddev,
    std::mt19937_64& rng)
{
    std::vector<seq::dna4> mutated_sequence;
    mutated_sequence.reserve(sequence.size() + sequence.size() / 8);
    std::uniform_real_distribution<double> probability(0.0, 1.0);
    std::uniform_int_distribution<int> base_dist(0, 3);
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
                seq::dna4 inserted_letter;
                inserted_letter.assign_rank(
                    static_cast<seqan3::alphabet_rank_t<seq::dna4>>(base_dist(rng)));
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

/// Clamp a normally-distributed fragment length into [1, upper_bound-1]
/// so uniform start-position sampling is always well-defined.
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

/**
 * @brief Generate a fresh batch of (query, target) pairs.
 *
 * Builds one synthetic reference, then repeatedly samples fragments from
 * it, optionally reverse-complements, injects SEAL-style substitution
 * and indel errors, and records the ground-truth origin so an evaluator
 * can classify each downstream aligner hit.
 *
 * The RNG is seeded once from params.seed so a given seed always yields
 * the same batch.
 */
[[nodiscard]] inline std::vector<GeneratedBenchSequencePair>
generate_test_sequences(const GeneratorParams& params) {
    std::mt19937_64 rng(params.seed);

    const std::vector<seq::dna4> reference =
        detail::build_random_reference(params.reference_length, rng);

    std::vector<GeneratedBenchSequencePair> pairs;
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

        std::vector<seq::dna4> fragment(
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_start),
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_end));

        const bool is_reverse =
            probability(rng) < params.reverse_complement_rate;
        if (is_reverse) {
            detail::reverse_complement_in_place(fragment);
        }

        detail::apply_uniform_substitutions(fragment, params.substitution_rate, rng);
        fragment = detail::apply_indels(
            fragment,
            params.insertion_rate, params.insertion_length_mean, params.insertion_length_stddev,
            params.deletion_rate,  params.deletion_length_mean,  params.deletion_length_stddev,
            rng);

        GeneratedBenchSequencePair pair;
        pair.query = bench_sequence_range(std::move(fragment));
        pair.target = bench_sequence_range(std::vector<seq::dna4>(reference));
        pair.truth_target_start = fragment_start;
        pair.truth_target_end   = fragment_end;
        pair.is_reverse         = is_reverse;
        pair.effective_error_rate =
            params.substitution_rate + params.insertion_rate + params.deletion_rate;
        pair.original_query_length = fragment_length;
        pairs.push_back(std::move(pair));
    }

    JustLog("[bench.generator] Generated {} fragment pairs over a {} bp reference "
            "(sub_rate={}, ins_rate={}, del_rate={})",
            pairs.size(), params.reference_length,
            params.substitution_rate, params.insertion_rate, params.deletion_rate);
    return pairs;
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
