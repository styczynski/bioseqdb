/**
 * @file gatekeeper.cppm
 * @brief GateKeeper sequence filtering on CPU.
 *
 * Compares equal-length inputs through shifted mismatch masks. Short internal
 * match runs are changed to mismatches before the masks are combined and scored.
 * An exact edit-distance check confirms mask-based rejections. Unequal lengths
 * use their length difference, and unsupported inputs remain candidates.
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
import bioseq.storage.lazy_sequence;
import bioseq.sequences.edit_distance_threshold;
import bioseq.prefilter.types;

namespace bioseq::prefilter::gatekeeper_detail {

/** Maximum equal-length rank count represented by one GateKeeper mask. */
inline constexpr std::size_t k_maximum_length = 128u;

/** Maximum edit threshold for which all shifted GateKeeper diagonals are evaluated. */
inline constexpr std::uint32_t k_maximum_edits = 8u;

/** One mismatch bit per supported sequence position. */
using mask_type = std::bitset<k_maximum_length>;

/**
 * @brief Marks the sequence positions used in a mismatch  mask.
 * @param length Number of positions, at most k_maximum_length.
 * @return Mask with the first length bits set.
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
 * @brief Marks unequal ranks at matching query and target positions.
 * @param query Query ranks.
 * @param target Target ranks.
 * @return Mismatch mask
 * @pre Equal lengths of at most k_maximum_length.
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
 * @brief Marks mismatches after shifting the query relative to the target.
 * @param query Query ranks.
 * @param target Target ranks.
 * @param distance Shift in symbols.
 * @param shift_query_right Shift right when true, ledt otherwise.
 * @return Mismatch bits with uncovered positions left zero.
 * @pre Equal lengths of at most k_maximum_length, with distance no larger than the length.
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
 * @brief Fills one- or two-symbol match gaps bounded by mismatches.
 * @param mismatches Mismatch bits for one shift.
 * @param valid Bits belonging to the sequence.
 * @return Amended mismatch mask within the valid positions.
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
 * @brief Reads four consecutive mask bits, starting with  the lowest position.
 * @param mask Source mask.
 * @param first_position First position to read.
 * @param length Sequence length.
 * @return Four-bit value, with positions beyond length left zero.
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
 * @brief Returns GateKeeper's score for a four-bit mismatch pattern.
 *
 * An empty pattern scores zero. Selected, mixed patterns score two, and other
 * nonempty patterns score one.
 * @param nibble Four-bit pattern.
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
 * @brief Sums four-bit mask scores until they exceed the allowed edits.
 * @param final_mask Combined GateKeeper mismatch mask.
 * @param length Number of sequence positions.
 * @param max_edits Edit threshold.
 * @return Score, capped at max_edits + 1.
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
 * @brief Checks whether a job fits the scalar GateKeeper mask limits.
 * @return True  for nonempty equal lengths up to 128 symbols and at most eight edits.
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
 * @brief Tests whether a rank pair can be rejected by GateKeeper.
 *
 * Passing this filter keeps the pair for alignment. It does not establish a match.
 * Equal-length inputs use shifted mimatch masks, and an exact edit-distance
 * check confirms any rejection proposed by those masks. Unequal lengths use
 * only their length difference Unsupported sizes keep the candidate.
 * @param query_ranks Query ranks in sequence order.
 * @param target_ranks Target ranks using the same encoding.
 * @param max_edits Allowed edit  distance.
 * @return Keep/reject decision, a safe lower bound, and optional mask diagnostics.
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
 * @brief Evaluates rank-buffer jobs in their original order.
 * @param input Query/target rank buffers and slices.
 * @return One result per job. Out-of-bounds slices are kept without evaluation.
 */
[[nodiscard]] inline prefilter_batch_result gatekeeper_scalar_batch(
    const prefilter_batch_input& input)
{
    return evaluate_prefilter_batch(input,
        [](const prefilter_rank_window& query, const prefilter_rank_window& target,
           const prefilter_job& job) {
            if (job.query_length != job.target_length) {
                const auto difference = job.query_length > job.target_length
                    ? job.query_length - job.target_length : job.target_length - job.query_length;
                return make_prefilter_result(difference, prefilter_status::conservative_unequal,
                    difference <= job.max_edits);
            }
            if (job.query_length == 0u) {
                return make_prefilter_result(0u, prefilter_status::success, true);
            }
            if (job.query_length > gatekeeper_detail::k_maximum_length ||
                job.max_edits > gatekeeper_detail::k_maximum_edits) {
                return unsupported_prefilter_result();
            }
            const auto target_ranks = target.window(0u, job.target_length);
            const auto query_ranks = query.window(0u, job.query_length);
            if (query.failure->has_value()) return unsupported_prefilter_result();
            return gatekeeper_scalar(query_ranks, target_ranks, job.max_edits);
        });
}

/**
 * @brief Decodes two compact sequences and applies scalar GateKeeper.
 *
 * Both ranges must use the same alphabet type so equal ranks mean equal symbols.
 * @param query Query sequence.
 * @param target Target sequence.
 * @param max_edits Allowed edit distance.
 * @return GateKeeper decision and diagnostics.
 */
template <supports_compact_sequence_range p_query_range_type,
          supports_compact_sequence_range p_target_range_type>
[[nodiscard]] inline std::expected<prefilter_result, prefilter_input_failure> gatekeeper_scalar_compact(
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

    if (query.bp_size() > (std::numeric_limits<std::uint32_t>::max)() ||
        target.bp_size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return unsupported_prefilter_result();
    }
    const std::array query_sources{storage::make_borrowed_rank_source(query)};
    const std::array target_sources{storage::make_borrowed_rank_source(target)};
    const std::array jobs{prefilter_job{
        .query_length = static_cast<std::uint32_t>(query.bp_size()),
        .target_length = static_cast<std::uint32_t>(target.bp_size()),
        .max_edits = max_edits,
    }};
    auto results = gatekeeper_scalar_batch({query_sources, target_sources, jobs});
    if (!results) return std::unexpected(results.error());
    return results->front();
}

} // namespace bioseq::prefilter
