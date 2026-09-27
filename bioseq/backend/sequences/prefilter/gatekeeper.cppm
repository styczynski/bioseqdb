/**
 * @file gatekeeper.cppm
 * @brief Implements the scalar GateKeeper oracle and its rank-range adapters.
 *
 * Equal-length inputs of at most 128 ranks and thresholds of at most eight edits use the
 * GateKeeper shifted-mismatch masks. Internal match runs of one or two ranks are amended before
 * the masks are intersected and scored as low-to-high four-bit groups. Every mask-based rejection
 * is confirmed by the exact threshold predicate, so unsupported geometry and unequal lengths
 * remain conservative.
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

/** Maximum equal-length rank count represented by one GateKeeper mask. */
inline constexpr std::size_t k_maximum_length = 128u;

/** Maximum edit threshold for which all shifted GateKeeper diagonals are evaluated. */
inline constexpr std::uint32_t k_maximum_edits = 8u;

/** Fixed-width logical mask covering every supported rank position. */
using mask_type = std::bitset<k_maximum_length>;

/**
 * @brief Builds the validity mask for one logical rank span.
 * @param[in] length Number of low-order positions belonging to the span.
 * @return A mask with exactly the first `length` positions set.
 * @pre `length` is no greater than @ref k_maximum_length.
 */
[[nodiscard]] inline mask_type valid_mask(std::size_t length)
{
    mask_type valid{};
    for (std::size_t position = 0u; position < length; ++position) {
        valid.set(position);
    }
    return valid;
}

/**
 * @brief Compares two unshifted rank spans by numeric-rank equality.
 * @param[in] query Query ranks in logical order.
 * @param[in] target Target ranks aligned to the query.
 * @return A mask whose set positions identify mismatching ranks.
 * @pre Both spans have the same size, no greater than @ref k_maximum_length.
 */
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
 * @brief Compares one shifted query-to-target diagonal.
 * @param[in] query Query ranks in logical order.
 * @param[in] target Target ranks in logical order.
 * @param[in] distance Positive diagonal displacement in ranks.
 * @param[in] shift_query_right Selects the query-right diagonal when `true` and the query-left
 * diagonal otherwise.
 * @return A mismatch mask with vacant diagonal positions left clear.
 * @pre The spans have equal size and `distance` does not exceed that size.
 *
 * Vacant positions stay zero rather than borrowing a sentinel value from the alphabet's rank
 * domain.
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

/**
 * @brief Amends bounded internal match runs of length one or two into mismatches.
 * @param[in] mismatches Mismatch mask for one diagonal.
 * @param[in] valid Mask identifying logical positions in the rank span.
 * @return The amended mismatch mask, restricted to valid positions.
 */
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

/**
 * @brief Extracts one logical low-to-high four-bit group from a mask.
 * @param[in] mask Mask to sample.
 * @param[in] first_position First logical position in the group.
 * @param[in] length Logical mask length. Positions beyond it remain clear.
 * @return The four-bit group with `first_position` in its least-significant bit.
 */
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

/**
 * @brief Computes the GateKeeper cost of one four-bit group without a lookup table.
 * @param[in] nibble Four-bit mismatch group.
 * @return Zero for an empty group, two for the six specified two-cost patterns, and one for every
 * other nonzero pattern.
 */
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

/**
 * @brief Accumulates final-mask group costs in the CPU/GPU-defined order.
 * @param[in] final_mask Intersected and amended GateKeeper mask.
 * @param[in] length Number of logical positions represented by the mask.
 * @param[in] max_edits Threshold used to cap the estimate.
 * @return The accumulated estimate, or `max_edits + 1` immediately after the threshold is
 * exceeded.
 */
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

/**
 * @brief Reports whether a job can use the direct scalar GateKeeper geometry.
 * @param[in] job Rank-slice geometry and edit threshold to validate.
 * @return `true` for nonempty equal lengths up to 128 ranks and thresholds up to eight edits.
 */
[[nodiscard]] constexpr inline bool gatekeeper_geometry_supported(
    const prefilter_job& job) noexcept
{
    return job.query_length == job.target_length
        && job.query_length != 0u
        && job.query_length <= gatekeeper_detail::k_maximum_length
        && job.max_edits <= gatekeeper_detail::k_maximum_edits;
}

/**
 * @brief Evaluates one pair of materialized rank spans with scalar GateKeeper masks.
 * @param[in] query_ranks Query ranks in logical order.
 * @param[in] target_ranks Target ranks in logical order.
 * @param[in] max_edits Maximum accepted edit distance.
 * @return A conservative prefilter result with GateKeeper diagnostics when the geometry is
 * supported.
 *
 * Unequal lengths return their exact length difference under the conservative-unequal status.
 * Supported equal lengths use the GateKeeper mask estimate, and every candidate rejection is
 * confirmed by the exact threshold predicate before it becomes observable.
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

/**
 * @brief Evaluates a flat rank batch in job order after validating every slice.
 * @param[in] input Flat query/target rank buffers and ordered jobs.
 * @return One result per input job in the original order.
 *
 * An out-of-bounds job produces an unsupported conservative result without loading its slices.
 */
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
 * @brief Materializes two compact ranges as ranks and delegates to the scalar core.
 * @tparam p_query_range_type Query range representation accepted by `compact_sequence_range`.
 * @tparam p_target_range_type Target range representation accepted by `compact_sequence_range`.
 * @param[in] query Compact query sequence.
 * @param[in] target Compact target sequence.
 * @param[in] max_edits Maximum accepted edit distance.
 * @return The scalar GateKeeper result for the two materialized rank spans.
 *
 * Numeric ranks are compared only after the two compact ranges are proven to use the identical
 * alphabet type.
 */
template <supports_compact_sequence_range p_query_range_type,
          supports_compact_sequence_range p_target_range_type>
[[nodiscard]] inline prefilter_result gatekeeper_scalar_compact(
    const compact_sequence_range<p_query_range_type>& query,
    const compact_sequence_range<p_target_range_type>& target,
    std::uint32_t max_edits)
{
    using query_alphabet =
        typename compact_sequence_range<p_query_range_type>::traits::alphabet_type;
    using target_alphabet =
        typename compact_sequence_range<p_target_range_type>::traits::alphabet_type;
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
