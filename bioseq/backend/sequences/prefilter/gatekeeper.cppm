/**
 * @file gatekeeper.cppm
 * @brief Clean-room scalar GateKeeper oracle for short equal-length ranks.
 */
module;

#include <bitset>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

export module bioseq.prefilter.gatekeeper;

import bioseq.sequences;
import bioseq.sequences.edit_distance_threshold;
import bioseq.prefilter.types;

namespace bioseq::prefilter::gatekeeper_detail {

inline constexpr std::size_t k_maximum_length = 128u;
inline constexpr std::uint32_t k_maximum_edits = 8u;

using mask_type = std::bitset<k_maximum_length>;

/** Return a mask whose first length logical bits are set. */
[[nodiscard]] inline mask_type valid_mask(std::size_t length)
{
    mask_type valid{};
    for (std::size_t position = 0u; position < length; ++position) {
        valid.set(position);
    }
    return valid;
}

/** Compare the two unshifted rank spans by equality only. */
[[nodiscard]] inline mask_type mismatch_mask(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target)
{
    mask_type mismatches{};
    for (std::size_t position = 0u; position < query.size(); ++position) {
        if (query[position] != target[position]) {
            mismatches.set(position);
        }
    }
    return mismatches;
}

/**
 * Compare one shifted diagonal. Vacant positions stay zero and therefore do
 * not borrow a value from the alphabet's rank domain.
 */
[[nodiscard]] inline mask_type shifted_mismatch_mask(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::size_t distance,
    bool shift_query_right)
{
    mask_type mismatches{};
    const std::size_t length = query.size();
    if (shift_query_right) {
        for (std::size_t position = distance; position < length; ++position) {
            if (query[position - distance] != target[position]) {
                mismatches.set(position);
            }
        }
    } else {
        for (std::size_t position = 0u;
             position + distance < length;
             ++position) {
            if (query[position + distance] != target[position]) {
                mismatches.set(position);
            }
        }
    }
    return mismatches;
}

/** Set bounded internal zero runs of length one or two. */
[[nodiscard]] inline mask_type amend_short_zero_runs(
    const mask_type& mismatches,
    const mask_type& valid)
{
    const mask_type matches = (~mismatches) & valid;
    const mask_type single = matches
        & (mismatches << 1u)
        & (mismatches >> 1u);
    const mask_type pair_start = matches
        & (matches >> 1u)
        & (mismatches << 1u)
        & (mismatches >> 2u);
    return (mismatches | single | pair_start | (pair_start << 1u)) & valid;
}

/** Extract one logical low-to-high four-bit group. */
[[nodiscard]] inline std::uint8_t nibble_at(
    const mask_type& mask,
    std::size_t first_position,
    std::size_t length) noexcept
{
    std::uint8_t nibble = 0u;
    for (std::size_t bit = 0u;
         bit < 4u && first_position + bit < length;
         ++bit) {
        if (mask.test(first_position + bit)) {
            nibble |= static_cast<std::uint8_t>(1u << bit);
        }
    }
    return nibble;
}

/** Return the GateKeeper cost of one four-bit group without a lookup table. */
[[nodiscard]] constexpr inline std::uint32_t nibble_cost(
    std::uint8_t nibble) noexcept
{
    switch (nibble) {
        case 0b0101u:
        case 0b0110u:
        case 0b1001u:
        case 0b1010u:
        case 0b1011u:
        case 0b1101u:
            return 2u;
        case 0u:
            return 0u;
        default:
            return 1u;
    }
}

/** Accumulate final-mask nibbles in the CPU/GPU-defined order and cap early. */
[[nodiscard]] inline std::uint32_t capped_mask_estimate(
    const mask_type& final_mask,
    std::size_t length,
    std::uint32_t max_edits) noexcept
{
    std::uint32_t estimate = 0u;
    for (std::size_t first_position = 0u;
         first_position < length;
         first_position += 4u) {
        estimate += nibble_cost(
            nibble_at(final_mask, first_position, length));
        if (estimate > max_edits) {
            return max_edits + 1u;
        }
    }
    return estimate;
}

} // namespace bioseq::prefilter::gatekeeper_detail

export namespace bioseq::prefilter {

/** Return whether a job's equal-length geometry can run the direct oracle. */
[[nodiscard]] constexpr inline bool gatekeeper_geometry_supported(
    const prefilter_job& job) noexcept
{
    return job.query_length == job.target_length
        && job.query_length != 0u
        && job.query_length <= gatekeeper_detail::k_maximum_length
        && job.max_edits <= gatekeeper_detail::k_maximum_edits;
}

/**
 * Evaluate one pair of materialized rank spans with the scalar GateKeeper
 * mask, followed by an exact threshold guard for every candidate rejection.
 */
[[nodiscard]] inline prefilter_result gatekeeper_scalar(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::uint32_t max_edits)
{
    if (query_ranks.size() != target_ranks.size()) {
        const std::size_t difference = query_ranks.size() > target_ranks.size()
            ? query_ranks.size() - target_ranks.size()
            : target_ranks.size() - query_ranks.size();
        if (difference
            > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return unsupported_prefilter_result();
        }
        const auto exact_difference = static_cast<std::uint32_t>(difference);
        return make_prefilter_result(
            exact_difference,
            prefilter_status::conservative_unequal,
            exact_difference <= max_edits);
    }

    if (query_ranks.empty()) {
        return make_prefilter_result(0u, prefilter_status::success, true);
    }

    if (query_ranks.size() > gatekeeper_detail::k_maximum_length
        || max_edits > gatekeeper_detail::k_maximum_edits
        || max_edits
            == (std::numeric_limits<std::uint32_t>::max)()) {
        return unsupported_prefilter_result();
    }

    const std::size_t length = query_ranks.size();
    const gatekeeper_detail::mask_type valid =
        gatekeeper_detail::valid_mask(length);
    const gatekeeper_detail::mask_type hamming =
        gatekeeper_detail::mismatch_mask(query_ranks, target_ranks);
    const std::uint32_t hamming_count = static_cast<std::uint32_t>(
        hamming.count());

    if (hamming_count <= max_edits) {
        return make_prefilter_result(
            0u, prefilter_status::success, true, hamming_count);
    }

    if (max_edits == 0u) {
        return make_prefilter_result(
            1u,
            prefilter_status::success,
            false,
            1u | k_gatekeeper_hamming_exact_rejection_mask);
    }

    gatekeeper_detail::mask_type final_mask =
        gatekeeper_detail::amend_short_zero_runs(hamming, valid);
    for (std::size_t distance = 1u;
         distance <= static_cast<std::size_t>(max_edits);
         ++distance) {
        gatekeeper_detail::mask_type amended_right =
            gatekeeper_detail::amend_short_zero_runs(
                gatekeeper_detail::shifted_mismatch_mask(
                    query_ranks, target_ranks, distance, true),
                valid);
        amended_right.set(distance - 1u);
        amended_right &= valid;
        final_mask &= amended_right;

        gatekeeper_detail::mask_type amended_left =
            gatekeeper_detail::amend_short_zero_runs(
                gatekeeper_detail::shifted_mismatch_mask(
                    query_ranks, target_ranks, distance, false),
                valid);
        amended_left.set(length - distance);
        amended_left &= valid;
        final_mask &= amended_left;
    }

    const std::uint32_t capped_estimate =
        gatekeeper_detail::capped_mask_estimate(
            final_mask, length, max_edits);
    if (capped_estimate <= max_edits) {
        return make_prefilter_result(
            0u,
            prefilter_status::success,
            true,
            capped_estimate);
    }

    const std::uint32_t candidate = capped_estimate
        | k_gatekeeper_mask_rejection_candidate_mask
        | k_gatekeeper_exact_guard_activated_mask;
    if (!bioseq::sequences::edit_distance_within_threshold(
            query_ranks, target_ranks, max_edits)) {
        return make_prefilter_result(
            max_edits + 1u,
            prefilter_status::success,
            false,
            candidate | k_gatekeeper_guard_confirmed_rejection_mask);
    }
    return make_prefilter_result(
        0u, prefilter_status::success, true, candidate);
}

/** Evaluate a flat rank batch in job order, validating slices before loads. */
[[nodiscard]] inline std::vector<prefilter_result> gatekeeper_scalar_batch(
    const prefilter_batch_input& input)
{
    std::vector<prefilter_result> results;
    results.reserve(input.jobs.size());
    for (const prefilter_job& job : input.jobs) {
        if (!prefilter_job_in_bounds(input, job)) {
            results.push_back(unsupported_prefilter_result());
            continue;
        }
        const std::span<const std::uint8_t> query =
            input.query_ranks.subspan(job.query_offset, job.query_length);
        const std::span<const std::uint8_t> target =
            input.target_ranks.subspan(job.target_offset, job.target_length);
        results.push_back(gatekeeper_scalar(query, target, job.max_edits));
    }
    return results;
}

/**
 * Materialize two compact ranges through their safe rank conversion and
 * delegate to the scalar rank core. Numeric ranks are compared only after the
 * alphabet types are proven identical.
 */
template <supports_compact_sequence_range query_range_type,
          supports_compact_sequence_range target_range_type>
[[nodiscard]] inline prefilter_result gatekeeper_scalar_compact(
    const compact_sequence_range<query_range_type>& query,
    const compact_sequence_range<target_range_type>& target,
    std::uint32_t max_edits)
{
    using query_alphabet =
        typename compact_sequence_range<query_range_type>::traits::alphabet_type;
    using target_alphabet =
        typename compact_sequence_range<target_range_type>::traits::alphabet_type;
    static_assert(
        std::same_as<query_alphabet, target_alphabet>,
        "GateKeeper compact operands must have identical alphabets");

    const std::vector<std::uint8_t> query_ranks = query.to_encoded_vector();
    const std::vector<std::uint8_t> target_ranks = target.to_encoded_vector();
    return gatekeeper_scalar(
        std::span<const std::uint8_t>{query_ranks},
        std::span<const std::uint8_t>{target_ranks},
        max_edits);
}

} // namespace bioseq::prefilter
