/**
 * @file gatekeeper_test.cpp
 * @brief Check GateKeeper results against independent edit-distance and mask calculations.
 *
 * Tests cover packed alphabets and byte ranks, sequence boundaries, invalid
 * slices, and batches. Valid matches must survive the filter.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.edit_distance_threshold;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.types;

using namespace seq::literals;
using namespace bioseq::prefilter;

namespace {

/** One byte per mismatch bit for the test's mask calculations. */
using bit_vector = std::vector<std::uint8_t>;

/**
 * @brief Compute exact edit distance for an independent test reference.
 *
 * Insertions, deletions, and substitutions each cost one. The calculation
 * keeps two rows of the dynamic-programming matrix.
 * @param[in] query Query ranks in sequence order.
 * @param[in] target Target ranks in sequence order.
 * @return Global Levenshtein distance.
 */
[[nodiscard]] inline std::uint32_t exact_levenshtein(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target)
{
    std::vector<std::uint32_t> previous(target.size() + 1u);
    std::vector<std::uint32_t> current(target.size() + 1u);
    for (std::size_t column = 0u; column <= target.size(); ++column) {
        previous[column] = static_cast<std::uint32_t>(column);
    }
    for (std::size_t row = 1u; row <= query.size(); ++row) {
        current[0] = static_cast<std::uint32_t>(row);
        for (std::size_t column = 1u; column <= target.size(); ++column) {
            const std::uint32_t substitution = previous[column - 1u]
                + (query[row - 1u] == target[column - 1u] ? 0u : 1u);
            current[column] = std::min({
                static_cast<std::uint32_t>(previous[column] + 1u),
                static_cast<std::uint32_t>(current[column - 1u] + 1u),
                substitution,
            });
        }
        previous.swap(current);
    }
    return previous.back();
}

/**
 * @brief Generate every binary sequence of the requested length.
 * @param[in] length Number of ranks per sequence.
 * @return Sequences ordered by their integer bit pattern, lowest bit first.
 */
[[nodiscard]] inline std::vector<std::vector<std::uint8_t>>
binary_sequences(std::uint32_t length)
{
    std::vector<std::vector<std::uint8_t>> sequences;
    const std::uint32_t count = 1u << length;
    sequences.reserve(count);
    for (std::uint32_t bits = 0u; bits < count; ++bits) {
        std::vector<std::uint8_t> sequence(length);
        for (std::uint32_t position = 0u; position < length; ++position) {
            sequence[position] = static_cast<std::uint8_t>(
                (bits >> position) & 1u);
        }
        sequences.push_back(std::move(sequence));
    }
    return sequences;
}

/**
 * @brief Fill match runs of length one or two when mismatches surround them.
 *
 * This scan gives an independent check of GateKeeper's mask amendment.
 * @param[in] mismatches One diagonal, with 1 for a mismatch and 0 for a match.
 * @return A copy with the short internal match runs changed to 1.
 */
[[nodiscard]] inline bit_vector amend_run_oracle(
    const bit_vector& mismatches)
{
    bit_vector amended = mismatches;
    std::size_t position = 0u;
    while (position < mismatches.size()) {
        if (mismatches[position] != 0u) {
            ++position;
            continue;
        }
        const std::size_t run_start = position;
        while (position < mismatches.size()
               && mismatches[position] == 0u) {
            ++position;
        }
        const std::size_t run_length = position - run_start;
        if (run_start != 0u
            && position < mismatches.size()
            && run_length <= 2u) {
            std::fill(
                amended.begin() + static_cast<std::ptrdiff_t>(run_start),
                amended.begin() + static_cast<std::ptrdiff_t>(position),
                1u);
        }
    }
    return amended;
}

/**
 * @brief Compare ranks along a shifted diagonal for the mask reference.
 * @param[in] query Query ranks in sequence order.
 * @param[in] target Target ranks in sequence order.
 * @param[in] distance Shift in ranks.
 * @param[in] shift_query_right Use query[position - distance] when true.
 * @return Mismatch bits, with zeros where the shifted query is out of range.
 */
[[nodiscard]] inline bit_vector shifted_mismatch_oracle(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::size_t distance,
    bool shift_query_right)
{
    bit_vector mismatches(query.size(), 0u);
    for (std::size_t position = 0u; position < query.size(); ++position) {
        if (shift_query_right) {
            if (position >= distance) {
                mismatches[position] = static_cast<std::uint8_t>(
                    query[position - distance] != target[position]);
            }
        } else if (position + distance < query.size()) {
            mismatches[position] = static_cast<std::uint8_t>(
                query[position + distance] != target[position]);
        }
    }
    return mismatches;
}

/**
 * @brief Check the GateKeeper cost of a four-bit mismatch group independently.
 * @param[in] nibble Four mismatch bits.
 * @return The group's cost: zero, one, or two edits.
 */
[[nodiscard]] constexpr inline std::uint32_t nibble_cost_oracle(
    std::uint8_t nibble) noexcept
{
    if (nibble == 0u) {
        return 0u;
    }
    return nibble == 0b0101u
        || nibble == 0b0110u
        || nibble == 0b1001u
        || nibble == 0b1010u
        || nibble == 0b1011u
        || nibble == 0b1101u
        ? 2u
        : 1u;
}

/**
 * @brief Calculate GateKeeper's diagnostic estimate with the test mask helpers.
 *
 * The reference scans individual bits and match runs, so it can check the
 * implementation's packed-word shifts and amendment.
 * @param[in] query Query ranks in sequence order.
 * @param[in] target Target ranks in sequence order.
 * @param[in] max_edits Edit limit.
 * @return Estimate capped at max_edits + 1.
 */
[[nodiscard]] inline std::uint32_t diagnostic_estimate_oracle(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits)
{
    bit_vector hamming(query.size(), 0u);
    std::uint32_t hamming_count = 0u;
    for (std::size_t position = 0u; position < query.size(); ++position) {
        hamming[position] = static_cast<std::uint8_t>(
            query[position] != target[position]);
        hamming_count += hamming[position];
    }
    if (hamming_count <= max_edits) {
        return hamming_count;
    }
    if (max_edits == 0u) {
        return 1u;
    }

    bit_vector final_mask = amend_run_oracle(hamming);
    for (std::size_t distance = 1u;
         distance <= static_cast<std::size_t>(max_edits);
         ++distance) {
        bit_vector right = amend_run_oracle(shifted_mismatch_oracle(
            query, target, distance, true));
        right[distance - 1u] = 1u;
        bit_vector left = amend_run_oracle(shifted_mismatch_oracle(
            query, target, distance, false));
        left[query.size() - distance] = 1u;
        for (std::size_t position = 0u;
             position < final_mask.size();
             ++position) {
            final_mask[position] = static_cast<std::uint8_t>(
                final_mask[position] & right[position] & left[position]);
        }
    }

    std::uint32_t estimate = 0u;
    for (std::size_t first_position = 0u;
         first_position < final_mask.size();
         first_position += 4u) {
        std::uint8_t nibble = 0u;
        for (std::size_t bit = 0u;
             bit < 4u && first_position + bit < final_mask.size();
             ++bit) {
            nibble |= static_cast<std::uint8_t>(
                final_mask[first_position + bit] << bit);
        }
        estimate += nibble_cost_oracle(nibble);
        if (estimate > max_edits) {
            return max_edits + 1u;
        }
    }
    return estimate;
}

/**
 * @brief Compare the lower bound, status, acceptance flag, and diagnostics.
 * @return True when all four fields match.
 */
[[nodiscard]] constexpr inline bool same_result(
    const prefilter_result& lhs,
    const prefilter_result& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.is_accepted == rhs.is_accepted
        && lhs.diagnostics == rhs.diagnostics;
}

/**
 * @brief Check that GateKeeper's diagnostic flags agree with each other.
 * @param[in] result Result from a supported equal-length job.
 * @param[in] max_edits Edit limit used for the job.
 */
inline void check_supported_diagnostics(
    const prefilter_result& result,
    std::uint32_t max_edits)
{
    REQUIRE((result.diagnostics & ~k_gatekeeper_diagnostic_valid_mask) == 0u);
    REQUIRE(gatekeeper_diagnostic_estimate(result) <= max_edits + 1u);
    REQUIRE(is_gatekeeper_mask_rejection_candidate(result)
            == is_gatekeeper_exact_guard_activated(result));
    if (is_gatekeeper_guard_confirmed_rejection(result)) {
        REQUIRE(is_gatekeeper_exact_guard_activated(result));
    }
    if (is_gatekeeper_hamming_exact_rejection(result)) {
        REQUIRE(max_edits == 0u);
        REQUIRE_FALSE(is_gatekeeper_exact_guard_activated(result));
    }
    if (is_gatekeeper_mask_rejection_candidate(result)) {
        REQUIRE(max_edits > 0u);
        REQUIRE(gatekeeper_diagnostic_estimate(result) == max_edits + 1u);
    }
}

/** Reproducible xorshift generator for test sequences. */
class deterministic_gatekeeper_ranks {
public:
    /** Start the test generator with the given seed. */
    explicit constexpr deterministic_gatekeeper_ranks(
        std::uint32_t seed) noexcept
        : state_(seed)
    {}

    /** Advance the generator and return the next 32-bit value. */
    [[nodiscard]] std::uint32_t next() noexcept
    {
        state_ ^= state_ << 13u;
        state_ ^= state_ >> 17u;
        state_ ^= state_ << 5u;
        return state_;
    }

    /**
     * @brief Generate a rank in [0, rank_count).
     * @pre rank_count is nonzero.
     */
    [[nodiscard]] std::uint8_t rank(std::uint32_t rank_count) noexcept
    {
        return static_cast<std::uint8_t>(next() % rank_count);
    }

private:
    std::uint32_t state_;
};

/**
 * @brief Generate a test sequence using the supplied random state.
 * @param[in] length Number of ranks.
 * @param[in] rank_count Ranks range from zero to rank_count - 1.
 * @param[in,out] ranks Generator advanced for each rank.
 * @pre rank_count is nonzero.
 */
[[nodiscard]] std::vector<std::uint8_t> randomized_rank_sequence(
    std::size_t length,
    std::uint32_t rank_count,
    deterministic_gatekeeper_ranks& ranks)
{
    std::vector<std::uint8_t> sequence(length);
    for (std::uint8_t& rank : sequence) {
        rank = ranks.rank(rank_count);
    }
    return sequence;
}

/**
 * @brief Change consecutive ranks, starting at a generated position.
 *
 * The positions wrap at the end. Each replacement differs from the old rank,
 * and at most target.size() positions change.
 * @param[in,out] target Sequence to change.
 * @param[in] rank_count Number of possible ranks.
 * @param[in] substitution_count Requested number of substitutions.
 * @param[in,out] ranks Generator for the starting position and replacements.
 * @pre rank_count is greater than one for a nonempty target.
 */
void apply_randomized_substitutions(
    std::vector<std::uint8_t>& target,
    std::uint32_t rank_count,
    std::size_t substitution_count,
    deterministic_gatekeeper_ranks& ranks)
{
    if (target.empty()) return;
    substitution_count = std::min(substitution_count, target.size());
    const std::size_t start = ranks.next() % target.size();
    for (std::size_t edit = 0u; edit < substitution_count; ++edit) {
        const std::size_t position = (start + edit) % target.size();
        const std::uint32_t delta =
            1u + ranks.next() % (rank_count - 1u);
        target[position] = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(target[position]) + delta)
            % rank_count);
    }
}

/**
 * @brief Make a target with a known kind of difference from the query.
 * @param[in] query Original ranks.
 * @param[in] rank_count Number of possible ranks, greater than one.
 * @param[in] max_edits Edit limit used to choose substitution counts.
 * @param[in] variant Select a copy, substitutions, a rotation, or random ranks.
 * @param[in,out] ranks Generator used by the chosen variant.
 * @return A target with the query's length.
 */
[[nodiscard]] std::vector<std::uint8_t> randomized_target_sequence(
    std::span<const std::uint8_t> query,
    std::uint32_t rank_count,
    std::uint32_t max_edits,
    std::uint32_t variant,
    deterministic_gatekeeper_ranks& ranks)
{
    std::vector<std::uint8_t> target(query.begin(), query.end());
    switch (variant % 5u) {
    case 0u:
        break;
    case 1u:
        apply_randomized_substitutions(
            target, rank_count, max_edits, ranks);
        break;
    case 2u:
        apply_randomized_substitutions(
            target,
            rank_count,
            static_cast<std::size_t>(max_edits) + 1u,
            ranks);
        break;
    case 3u:
        if (target.size() > 1u) {
            std::rotate(
                target.begin(), target.end() - 1, target.end());
        } else {
            apply_randomized_substitutions(
                target, rank_count, 1u, ranks);
        }
        break;
    default:
        target = randomized_rank_sequence(
            query.size(), rank_count, ranks);
        break;
    }
    return target;
}

/**
 * @brief Convert ranks to SeqAn alphabet values in the same order.
 * @tparam p_alphabet Alphabet containing every input rank.
 * @param[in] ranks Valid ranks for p_alphabet.
 */
template <typename p_alphabet>
[[nodiscard]] std::vector<p_alphabet> alphabet_sequence_from_ranks(
    std::span<const std::uint8_t> ranks)
{
    std::vector<p_alphabet> sequence;
    sequence.reserve(ranks.size());
    for (const std::uint8_t rank : ranks) {
        p_alphabet letter{};
        letter.assign_rank(
            static_cast<decltype(letter.to_rank())>(rank));
        sequence.push_back(letter);
    }
    return sequence;
}

/**
 * @brief Check a GateKeeper result against exact distance and the test mask reference.
 *
 * Verify the lower bound and diagnostic flags. Every sequence pair within
 * max_edits must be accepted.
 * @param[in] query Query ranks.
 * @param[in] target Target ranks.
 * @param[in] max_edits Edit limit used by GateKeeper.
 * @param[in] result Result to check.
 */
inline void check_randomized_gatekeeper_result(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    const prefilter_result& result)
{
    const std::uint32_t distance = exact_levenshtein(query, target);
    const bool exact_within_threshold =
        bioseq::sequences::edit_distance_within_threshold(
            query, target, max_edits);
    const std::uint32_t expected_estimate =
        diagnostic_estimate_oracle(query, target, max_edits);
    const bool expected_guard_activation =
        max_edits > 0u && expected_estimate > max_edits;
    const bool expected_rejection =
        max_edits == 0u
            ? distance > 0u
            : expected_guard_activation && distance > max_edits;
    CAPTURE(
        query.size(),
        max_edits,
        distance,
        exact_within_threshold,
        expected_estimate,
        result.lower_bound,
        result.diagnostics);

    REQUIRE(is_prefilter_evaluated(result));
    REQUIRE(exact_within_threshold == (distance <= max_edits));
    REQUIRE(result.lower_bound <= distance);
    check_supported_diagnostics(result, max_edits);
    REQUIRE(gatekeeper_diagnostic_estimate(result) == expected_estimate);
    REQUIRE(is_gatekeeper_mask_rejection_candidate(result)
            == expected_guard_activation);
    REQUIRE(is_gatekeeper_exact_guard_activated(result)
            == expected_guard_activation);
    REQUIRE(is_gatekeeper_guard_confirmed_rejection(result)
            == (expected_guard_activation && distance > max_edits));
    REQUIRE(is_gatekeeper_hamming_exact_rejection(result)
            == (max_edits == 0u && distance > 0u));
    REQUIRE(is_prefilter_accepted(result) == !expected_rejection);
    if (exact_within_threshold) {
        REQUIRE(is_prefilter_accepted(result));
    }
    if (!is_prefilter_accepted(result)) {
        REQUIRE(distance > max_edits);
        REQUIRE(result.lower_bound == max_edits + 1u);
    }
}

TEST_CASE(
    "GateKeeper ordered contract and diagnostics are exact",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    STATIC_REQUIRE(static_cast<std::uint32_t>(prefilter_status::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative) == 2u);
    STATIC_REQUIRE(gatekeeper_geometry_supported(prefilter_job{
        .query_length = 128u,
        .target_length = 128u,
        .max_edits = 8u,
    }));
    STATIC_REQUIRE_FALSE(gatekeeper_geometry_supported(prefilter_job{
        .query_length = 0u,
        .target_length = 0u,
        .max_edits = 0u,
    }));
    STATIC_REQUIRE_FALSE(gatekeeper_geometry_supported(prefilter_job{
        .query_length = 129u,
        .target_length = 129u,
        .max_edits = 2u,
    }));

    const std::array<std::uint8_t, 8u> query{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const prefilter_result identical = gatekeeper_scalar(query, query, 0u);
    REQUIRE(is_prefilter_evaluated(identical));
    REQUIRE(is_prefilter_accepted(identical));
    REQUIRE(identical.lower_bound == 0u);
    REQUIRE(identical.diagnostics == 0u);

    auto target = query;
    target[3] = 128u;
    const prefilter_result hamming_rejection =
        gatekeeper_scalar(query, target, 0u);
    REQUIRE(is_prefilter_evaluated(hamming_rejection));
    REQUIRE_FALSE(is_prefilter_accepted(hamming_rejection));
    REQUIRE(hamming_rejection.lower_bound == 1u);
    REQUIRE(gatekeeper_diagnostic_estimate(hamming_rejection) == 1u);
    REQUIRE(is_gatekeeper_hamming_exact_rejection(hamming_rejection));
    REQUIRE_FALSE(is_gatekeeper_mask_rejection_candidate(hamming_rejection));
    REQUIRE_FALSE(is_gatekeeper_exact_guard_activated(hamming_rejection));
    REQUIRE_FALSE(is_gatekeeper_guard_confirmed_rejection(hamming_rejection));

    const prefilter_result hamming_acceptance =
        gatekeeper_scalar(query, target, 1u);
    REQUIRE(is_prefilter_accepted(hamming_acceptance));
    REQUIRE(hamming_acceptance.lower_bound == 0u);
    REQUIRE(gatekeeper_diagnostic_estimate(hamming_acceptance) == 1u);
    REQUIRE_FALSE(is_gatekeeper_mask_rejection_candidate(hamming_acceptance));

    const std::array<std::uint8_t, 3u> shorter{1u, 2u, 3u};
    const std::array<std::uint8_t, 6u> longer{1u, 2u, 3u, 4u, 5u, 6u};
    const prefilter_result unequal_reject =
        gatekeeper_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.status == static_cast<std::uint32_t>(
        prefilter_status::conservative_unequal));
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(is_prefilter_accepted(unequal_reject));
    REQUIRE(unequal_reject.diagnostics == 0u);
    const prefilter_result unequal_accept =
        gatekeeper_scalar(shorter, longer, 3u);
    REQUIRE(is_prefilter_accepted(unequal_accept));
    REQUIRE(unequal_accept.lower_bound == 3u);

    const std::span<const std::uint8_t> empty{};
    const prefilter_result empty_result = gatekeeper_scalar(
        empty, empty, (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(empty_result.status == static_cast<std::uint32_t>(
        prefilter_status::success));
    REQUIRE(is_prefilter_accepted(empty_result));
    REQUIRE(empty_result.lower_bound == 0u);
    REQUIRE(empty_result.diagnostics == 0u);

    const std::array<std::uint8_t, 1u> one{0u};
    const prefilter_result unsupported_edits =
        gatekeeper_scalar(one, one, 9u);
    REQUIRE(unsupported_edits.status == static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative));
    REQUIRE(is_prefilter_accepted(unsupported_edits));
    REQUIRE(unsupported_edits.lower_bound == 0u);
    REQUIRE(unsupported_edits.diagnostics == 0u);

    const std::vector<std::uint8_t> too_long(129u, 7u);
    const prefilter_result unsupported_length =
        gatekeeper_scalar(too_long, too_long, 2u);
    REQUIRE(unsupported_length.status == static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative));
    REQUIRE(is_prefilter_accepted(unsupported_length));
    REQUIRE(unsupported_length.diagnostics == 0u);
}

TEST_CASE(
    "GateKeeper exact guard confirms mask rejection",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const std::vector<std::uint8_t> query(32u, 0u);
    const std::vector<std::uint8_t> target(32u, 255u);
    for (const std::uint32_t max_edits : {2u, 4u}) {
        const prefilter_result result =
            gatekeeper_scalar(query, target, max_edits);
        CAPTURE(max_edits, result.lower_bound, result.diagnostics);
        REQUIRE_FALSE(is_prefilter_accepted(result));
        REQUIRE(result.lower_bound == max_edits + 1u);
        REQUIRE(gatekeeper_diagnostic_estimate(result) == max_edits + 1u);
        REQUIRE(is_gatekeeper_mask_rejection_candidate(result));
        REQUIRE(is_gatekeeper_exact_guard_activated(result));
        REQUIRE(is_gatekeeper_guard_confirmed_rejection(result));
        REQUIRE_FALSE(is_gatekeeper_hamming_exact_rejection(result));
        check_supported_diagnostics(result, max_edits);
    }
}

TEST_CASE(
    "GateKeeper never falsely rejects exhaustive short binary pairs",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    for (std::uint32_t length = 0u; length <= 7u; ++length) {
        const auto sequences = binary_sequences(length);
        for (const auto& query : sequences) {
            for (const auto& target : sequences) {
                const std::uint32_t distance =
                    exact_levenshtein(query, target);
                for (std::uint32_t max_edits = 0u;
                     max_edits <= length;
                     ++max_edits) {
                    const prefilter_result result = gatekeeper_scalar(
                        query, target, max_edits);
                    CAPTURE(
                        length,
                        query,
                        target,
                        distance,
                        max_edits,
                        result.lower_bound,
                        result.diagnostics);
                    REQUIRE(is_prefilter_evaluated(result));
                    REQUIRE(result.lower_bound <= distance);
                    check_supported_diagnostics(result, max_edits);
                    REQUIRE(gatekeeper_diagnostic_estimate(result)
                            == diagnostic_estimate_oracle(
                                query, target, max_edits));
                    if (distance <= max_edits) {
                        REQUIRE(is_prefilter_accepted(result));
                    }
                    if (!is_prefilter_accepted(result)) {
                        REQUIRE(distance > max_edits);
                        REQUIRE(result.lower_bound == max_edits + 1u);
                        if (max_edits == 0u) {
                            REQUIRE(is_gatekeeper_hamming_exact_rejection(result));
                        } else {
                            REQUIRE(is_gatekeeper_guard_confirmed_rejection(result));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE(
    "GateKeeper shifted vacancies remain sound at mask boundaries",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    constexpr std::array<std::size_t, 13u> lengths{
        0u, 1u, 2u, 15u, 16u, 17u, 31u,
        32u, 33u, 63u, 64u, 65u, 128u};
    for (const std::size_t length : lengths) {
        std::vector<std::uint8_t> query(length);
        for (std::size_t position = 0u; position < length; ++position) {
            query[position] = static_cast<std::uint8_t>(
                (position * 73u + 19u) % 251u);
        }

        std::vector<std::uint8_t> shifted_right = query;
        std::vector<std::uint8_t> shifted_left = query;
        if (length > 2u) {
            std::move_backward(
                shifted_right.begin(),
                shifted_right.end() - 1,
                shifted_right.end());
            shifted_right.front() = 255u;
            std::move(
                shifted_left.begin() + 1,
                shifted_left.end(),
                shifted_left.begin());
            shifted_left.back() = 254u;
        }

        for (const auto* target : {&shifted_right, &shifted_left}) {
            const std::uint32_t distance = exact_levenshtein(query, *target);
            const prefilter_result result = gatekeeper_scalar(
                query, *target, 2u);
            CAPTURE(length, distance, result.lower_bound, result.diagnostics);
            REQUIRE(distance <= 2u);
            REQUIRE(is_prefilter_accepted(result));
            REQUIRE(result.lower_bound == 0u);
            check_supported_diagnostics(result, 2u);
            REQUIRE(gatekeeper_diagnostic_estimate(result)
                    == diagnostic_estimate_oracle(query, *target, 2u));
        }
    }
}

TEST_CASE(
    "GateKeeper amendment crosses every 32-bit and partial-word boundary",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    constexpr std::array<std::size_t, 8u> lengths{
        31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u};
    for (const std::size_t length : lengths) {
        const std::vector<std::uint8_t> query(length, 0u);
        std::vector<std::uint8_t> single_run_target(length, 1u);
        std::vector<std::uint8_t> pair_run_target(length, 1u);
        for (std::size_t boundary = 32u;
             boundary < length;
             boundary += 32u) {
            if (boundary + 1u < length) {
                single_run_target[boundary] = 0u;
                pair_run_target[boundary - 1u] = 0u;
                pair_run_target[boundary] = 0u;
            }
        }

        for (const auto* target : {&single_run_target, &pair_run_target}) {
            for (const std::uint32_t max_edits : {1u, 2u, 4u, 8u}) {
                const prefilter_result result = gatekeeper_scalar(
                    query, *target, max_edits);
                const std::uint32_t expected_estimate =
                    diagnostic_estimate_oracle(query, *target, max_edits);
                CAPTURE(
                    length,
                    max_edits,
                    expected_estimate,
                    result.lower_bound,
                    result.diagnostics);
                REQUIRE(gatekeeper_diagnostic_estimate(result)
                        == expected_estimate);
                check_supported_diagnostics(result, max_edits);
                const std::uint32_t distance =
                    exact_levenshtein(query, *target);
                REQUIRE(result.lower_bound <= distance);
                if (distance <= max_edits) {
                    REQUIRE(is_prefilter_accepted(result));
                }
            }
        }
    }
}

TEST_CASE(
    "GateKeeper batch validates slices before ordered fallbacks",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const std::vector<std::uint8_t> query_ranks(132u, 7u);
    const std::vector<std::uint8_t> target_ranks(132u, 7u);
    const std::array<prefilter_job, 5u> jobs{
        prefilter_job{
            .query_offset = (std::numeric_limits<std::uint32_t>::max)(),
            .target_offset = 0u,
            .query_length = 1u,
            .target_length = 1u,
            .max_edits = 0u,
        },
        prefilter_job{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 129u,
            .target_length = 128u,
            .max_edits = 0u,
        },
        prefilter_job{
            .query_offset = 132u,
            .target_offset = 132u,
            .query_length = 0u,
            .target_length = 0u,
            .max_edits = 9u,
        },
        prefilter_job{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 129u,
            .target_length = 129u,
            .max_edits = 2u,
        },
        prefilter_job{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 0u,
        },
    };
    const prefilter_batch_input input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto results = gatekeeper_scalar_batch(input);
    REQUIRE(results.size() == jobs.size());

    REQUIRE(results[0].status == static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative));
    REQUIRE(is_prefilter_accepted(results[0]));
    REQUIRE(results[0].lower_bound == 0u);
    REQUIRE(results[0].diagnostics == 0u);

    REQUIRE(results[1].status == static_cast<std::uint32_t>(
        prefilter_status::conservative_unequal));
    REQUIRE_FALSE(is_prefilter_accepted(results[1]));
    REQUIRE(results[1].lower_bound == 1u);
    REQUIRE(results[1].diagnostics == 0u);

    REQUIRE(results[2].status == static_cast<std::uint32_t>(
        prefilter_status::success));
    REQUIRE(is_prefilter_accepted(results[2]));
    REQUIRE(results[2].diagnostics == 0u);

    REQUIRE(results[3].status == static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative));
    REQUIRE(is_prefilter_accepted(results[3]));
    REQUIRE(results[3].diagnostics == 0u);

    const prefilter_result direct = gatekeeper_scalar(
        std::span<const std::uint8_t>{query_ranks}.subspan(1u, 32u),
        std::span<const std::uint8_t>{target_ranks}.subspan(1u, 32u),
        0u);
    REQUIRE(same_result(results[4], direct));
}

TEST_CASE(
    "GateKeeper compact adapter preserves dna4 dna5 and aa27 ranks",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    auto dna4_query_data = "ACGTACGTACGTACGT"_dna4;
    auto dna4_target_data = "ACGTACGTTCGTACGT"_dna4;
    compact_sequence_range<std::vector<seq::dna4>> dna4_query{
        dna4_query_data};
    compact_sequence_range<std::vector<seq::dna4>> dna4_target{
        dna4_target_data};
    const prefilter_result dna4_result = gatekeeper_scalar_compact(
        dna4_query, dna4_target, 1u);
    REQUIRE(is_prefilter_accepted(dna4_result));
    REQUIRE(gatekeeper_diagnostic_estimate(dna4_result) == 1u);

    auto dna5_query_data = "ACGTNACGTNACGTN"_dna5;
    auto dna5_target_data = "ACGTNACGTNNCGTN"_dna5;
    compact_sequence_range<std::vector<seq::dna5>> dna5_query{
        dna5_query_data};
    compact_sequence_range<std::vector<seq::dna5>> dna5_target{
        dna5_target_data};
    const prefilter_result dna5_result = gatekeeper_scalar_compact(
        dna5_query, dna5_target, 1u);
    REQUIRE(is_prefilter_accepted(dna5_result));
    REQUIRE(gatekeeper_diagnostic_estimate(dna5_result) == 1u);

    using amino_storage = sequence_custom_alphabet<seq::aa27, std::uint64_t>;
    std::vector<seq::aa27> amino_query_data(32u);
    std::vector<seq::aa27> amino_target_data(32u);
    for (std::size_t position = 0u;
         position < amino_query_data.size();
         ++position) {
        seq::aa27 letter{};
        letter.assign_rank(static_cast<decltype(letter.to_rank())>(
            position % 27u));
        amino_query_data[position] = letter;
        amino_target_data[position] = letter;
    }
    amino_target_data[17u].assign_rank(26u);
    compact_sequence_range<amino_storage> amino_query{amino_query_data};
    compact_sequence_range<amino_storage> amino_target{amino_target_data};
    const prefilter_result amino_result = gatekeeper_scalar_compact(
        amino_query, amino_target, 1u);
    REQUIRE(is_prefilter_accepted(amino_result));
    REQUIRE(gatekeeper_diagnostic_estimate(amino_result) == 1u);

    const auto amino_query_ranks = amino_query.to_encoded_vector();
    const auto amino_target_ranks = amino_target.to_encoded_vector();
    const prefilter_result rank_result = gatekeeper_scalar(
        amino_query_ranks, amino_target_ranks, 1u);
    REQUIRE(same_result(amino_result, rank_result));
}

TEST_CASE(
    "GateKeeper randomized packed compact adapters match exact distance",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    constexpr std::array<std::size_t, 14u> lengths{
        0u, 1u, 2u, 15u, 16u, 17u, 31u,
        32u, 33u, 63u, 64u, 65u, 127u, 128u};
    constexpr std::uint32_t variants_per_cell = 5u;

    const auto check_alphabet = [
        &lengths]<typename p_alphabet>(
            std::uint32_t rank_count,
            std::uint32_t seed,
            std::uint32_t alphabet_tag) {
        using packed_storage =
            sequence_custom_alphabet<p_alphabet, std::uint64_t>;
        deterministic_gatekeeper_ranks ranks{seed};
        std::size_t checked_cases = 0u;
        for (const std::size_t length : lengths) {
            for (std::uint32_t max_edits = 0u;
                 max_edits <= 8u;
                 ++max_edits) {
                for (std::uint32_t variant = 0u;
                     variant < variants_per_cell;
                     ++variant) {
                    const std::vector<std::uint8_t> query_ranks =
                        randomized_rank_sequence(
                            length, rank_count, ranks);
                    const std::vector<std::uint8_t> target_ranks =
                        randomized_target_sequence(
                            query_ranks,
                            rank_count,
                            max_edits,
                            variant,
                            ranks);
                    std::vector<p_alphabet> query_letters =
                        alphabet_sequence_from_ranks<p_alphabet>(query_ranks);
                    std::vector<p_alphabet> target_letters =
                        alphabet_sequence_from_ranks<p_alphabet>(target_ranks);

                    compact_sequence_range<packed_storage> query_compact{
                        query_letters};
                    compact_sequence_range<packed_storage> target_compact{
                        target_letters};
                    const std::vector<std::uint8_t> encoded_query =
                        query_compact.to_encoded_vector();
                    const std::vector<std::uint8_t> encoded_target =
                        target_compact.to_encoded_vector();
                    CAPTURE(
                        alphabet_tag,
                        length,
                        max_edits,
                        variant);
                    REQUIRE(encoded_query == query_ranks);
                    REQUIRE(encoded_target == target_ranks);

                    const prefilter_result compact_result =
                        gatekeeper_scalar_compact(
                            query_compact,
                            target_compact,
                            max_edits);
                    const prefilter_result rank_result = gatekeeper_scalar(
                        encoded_query,
                        encoded_target,
                        max_edits);
                    REQUIRE(same_result(compact_result, rank_result));
                    check_randomized_gatekeeper_result(
                        encoded_query,
                        encoded_target,
                        max_edits,
                        compact_result);
                    ++checked_cases;
                }
            }
        }
        REQUIRE(checked_cases
                == lengths.size() * 9u * variants_per_cell);
    };

    check_alphabet.template operator()<seq::dna4>(
        4u, 0xD4A40001u, 4u);
    check_alphabet.template operator()<seq::dna5>(
        5u, 0xD5A50001u, 5u);
    check_alphabet.template operator()<seq::aa27>(
        27u, 0xAA270001u, 27u);
}

TEST_CASE(
    "GateKeeper randomized arbitrary rank batches match exact distance",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    constexpr std::array<std::size_t, 14u> lengths{
        0u, 1u, 2u, 15u, 16u, 17u, 31u,
        32u, 33u, 63u, 64u, 65u, 127u, 128u};
    constexpr std::uint32_t variants_per_cell = 5u;
    struct randomized_case {
        std::size_t length;
        std::uint32_t max_edits;
        std::uint32_t variant;
    };

    deterministic_gatekeeper_ranks ranks{0xB17ECA5Eu};
    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<prefilter_job> jobs;
    std::vector<randomized_case> cases;
    bool observed_rank_outside_biological_alphabets = false;
    for (const std::size_t length : lengths) {
        for (std::uint32_t max_edits = 0u;
             max_edits <= 8u;
             ++max_edits) {
            for (std::uint32_t variant = 0u;
                 variant < variants_per_cell;
                 ++variant) {
                const std::vector<std::uint8_t> query =
                    randomized_rank_sequence(length, 256u, ranks);
                const std::vector<std::uint8_t> target =
                    randomized_target_sequence(
                        query,
                        256u,
                        max_edits,
                        variant,
                        ranks);
                observed_rank_outside_biological_alphabets |=
                    std::any_of(
                        query.begin(),
                        query.end(),
                        [](std::uint8_t rank) { return rank > 26u; })
                    || std::any_of(
                        target.begin(),
                        target.end(),
                        [](std::uint8_t rank) { return rank > 26u; });

                const std::size_t query_prefix =
                    1u + ranks.next() % 3u;
                const std::size_t target_prefix =
                    1u + ranks.next() % 3u;
                const std::uint8_t query_poison = ranks.rank(256u);
                const std::uint8_t target_poison = ranks.rank(256u);
                query_ranks.insert(
                    query_ranks.end(), query_prefix, query_poison);
                target_ranks.insert(
                    target_ranks.end(), target_prefix, target_poison);
                const std::uint32_t query_offset =
                    static_cast<std::uint32_t>(query_ranks.size());
                const std::uint32_t target_offset =
                    static_cast<std::uint32_t>(target_ranks.size());
                query_ranks.insert(
                    query_ranks.end(), query.begin(), query.end());
                target_ranks.insert(
                    target_ranks.end(), target.begin(), target.end());
                query_ranks.push_back(ranks.rank(256u));
                target_ranks.push_back(ranks.rank(256u));
                jobs.push_back(prefilter_job{
                    .query_offset = query_offset,
                    .target_offset = target_offset,
                    .query_length = static_cast<std::uint32_t>(length),
                    .target_length = static_cast<std::uint32_t>(length),
                    .max_edits = max_edits,
                });
                cases.push_back(randomized_case{
                    .length = length,
                    .max_edits = max_edits,
                    .variant = variant,
                });
            }
        }
    }

    const prefilter_batch_input input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    REQUIRE(jobs.size() == lengths.size() * 9u * variants_per_cell);
    REQUIRE(observed_rank_outside_biological_alphabets);
    const std::vector<prefilter_result> results =
        gatekeeper_scalar_batch(input);
    REQUIRE(results.size() == jobs.size());
    REQUIRE(cases.size() == jobs.size());
    for (std::size_t index = 0u; index < jobs.size(); ++index) {
        const prefilter_job& job = jobs[index];
        const std::span<const std::uint8_t> query =
            std::span<const std::uint8_t>{query_ranks}.subspan(
                job.query_offset, job.query_length);
        const std::span<const std::uint8_t> target =
            std::span<const std::uint8_t>{target_ranks}.subspan(
                job.target_offset, job.target_length);
        CAPTURE(
            index,
            cases[index].length,
            cases[index].max_edits,
            cases[index].variant,
            job.query_offset,
            job.target_offset);
        REQUIRE(prefilter_job_in_bounds(input, job));
        REQUIRE(same_result(
            results[index],
            gatekeeper_scalar(query, target, job.max_edits)));
        check_randomized_gatekeeper_result(
            query, target, job.max_edits, results[index]);
    }
}

} // namespace
