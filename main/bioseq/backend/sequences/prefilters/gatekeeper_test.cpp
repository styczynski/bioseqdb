/**
 * @file gatekeeper_test.cpp
 * @brief Focused semantic tests for the clean-room scalar GateKeeper oracle.
 */
#include "common.h"
#include "common_tests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.prefilters.gatekeeper;
import bioseq.sequences.prefilters.gatekeeper_vulkan;
import bioseq.sequences.prefilters.prefilter_types;

using namespace seq::literals;
using namespace bioseq::sequences::prefilters;

namespace {

using bit_vector = std::vector<std::uint8_t>;

/** Compute independent full-matrix global Levenshtein distance. */
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

/** Enumerate every binary string of one fixed length. */
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

/** Amend bounded zero runs by scanning runs, independently of word shifts. */
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

/** Build one shifted mismatch row with vacant positions represented by zero. */
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

/** Derive the specified nibble cost by comparisons rather than a table. */
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
 * Compute only the packed low diagnostic through a simple run-based mask
 * oracle. This intentionally does not share the implementation's bitset
 * amendment or multiword shifts.
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

/** Compare every result ABI field. */
[[nodiscard]] constexpr inline bool same_result(
    const PrefilterResult& lhs,
    const PrefilterResult& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.accepted == rhs.accepted
        && lhs.reserved == rhs.reserved;
}

/** Check the diagnostic implications shared by all supported equal jobs. */
inline void check_supported_diagnostics(
    const PrefilterResult& result,
    std::uint32_t max_edits)
{
    REQUIRE((result.reserved & ~gatekeeper_diagnostic_valid_mask) == 0u);
    REQUIRE(gatekeeper_diagnostic_estimate(result) <= max_edits + 1u);
    REQUIRE(gatekeeper_mask_rejection_candidate(result)
            == gatekeeper_exact_guard_activated(result));
    if (gatekeeper_guard_confirmed_rejection(result)) {
        REQUIRE(gatekeeper_exact_guard_activated(result));
    }
    if (gatekeeper_hamming_exact_rejection(result)) {
        REQUIRE(max_edits == 0u);
        REQUIRE_FALSE(gatekeeper_exact_guard_activated(result));
    }
    if (gatekeeper_mask_rejection_candidate(result)) {
        REQUIRE(max_edits > 0u);
        REQUIRE(gatekeeper_diagnostic_estimate(result) == max_edits + 1u);
    }
}

TEST_CASE(
    "GateKeeper ordered contract and diagnostics are exact",
    "[gatekeeper][prefilter][contract]")
{
    STATIC_REQUIRE(static_cast<std::uint32_t>(PrefilterStatus::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        PrefilterStatus::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative) == 2u);
    STATIC_REQUIRE(gatekeeper_geometry_supported(PrefilterJob{
        .query_length = 128u,
        .target_length = 128u,
        .max_edits = 8u,
    }));
    STATIC_REQUIRE_FALSE(gatekeeper_geometry_supported(PrefilterJob{
        .query_length = 0u,
        .target_length = 0u,
        .max_edits = 0u,
    }));
    STATIC_REQUIRE_FALSE(gatekeeper_geometry_supported(PrefilterJob{
        .query_length = 129u,
        .target_length = 129u,
        .max_edits = 2u,
    }));

    const std::array<std::uint8_t, 8u> query{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const PrefilterResult identical = gatekeeper_scalar(query, query, 0u);
    REQUIRE(prefilter_evaluated(identical));
    REQUIRE(prefilter_accepted(identical));
    REQUIRE(identical.lower_bound == 0u);
    REQUIRE(identical.reserved == 0u);

    auto target = query;
    target[3] = 128u;
    const PrefilterResult hamming_rejection =
        gatekeeper_scalar(query, target, 0u);
    REQUIRE(prefilter_evaluated(hamming_rejection));
    REQUIRE_FALSE(prefilter_accepted(hamming_rejection));
    REQUIRE(hamming_rejection.lower_bound == 1u);
    REQUIRE(gatekeeper_diagnostic_estimate(hamming_rejection) == 1u);
    REQUIRE(gatekeeper_hamming_exact_rejection(hamming_rejection));
    REQUIRE_FALSE(gatekeeper_mask_rejection_candidate(hamming_rejection));
    REQUIRE_FALSE(gatekeeper_exact_guard_activated(hamming_rejection));
    REQUIRE_FALSE(gatekeeper_guard_confirmed_rejection(hamming_rejection));

    const PrefilterResult hamming_acceptance =
        gatekeeper_scalar(query, target, 1u);
    REQUIRE(prefilter_accepted(hamming_acceptance));
    REQUIRE(hamming_acceptance.lower_bound == 0u);
    REQUIRE(gatekeeper_diagnostic_estimate(hamming_acceptance) == 1u);
    REQUIRE_FALSE(gatekeeper_mask_rejection_candidate(hamming_acceptance));

    const std::array<std::uint8_t, 3u> shorter{1u, 2u, 3u};
    const std::array<std::uint8_t, 6u> longer{1u, 2u, 3u, 4u, 5u, 6u};
    const PrefilterResult unequal_reject =
        gatekeeper_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.status == static_cast<std::uint32_t>(
        PrefilterStatus::conservative_unequal));
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(prefilter_accepted(unequal_reject));
    REQUIRE(unequal_reject.reserved == 0u);
    const PrefilterResult unequal_accept =
        gatekeeper_scalar(shorter, longer, 3u);
    REQUIRE(prefilter_accepted(unequal_accept));
    REQUIRE(unequal_accept.lower_bound == 3u);

    const std::span<const std::uint8_t> empty{};
    const PrefilterResult empty_result = gatekeeper_scalar(
        empty, empty, (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(empty_result.status == static_cast<std::uint32_t>(
        PrefilterStatus::success));
    REQUIRE(prefilter_accepted(empty_result));
    REQUIRE(empty_result.lower_bound == 0u);
    REQUIRE(empty_result.reserved == 0u);

    const std::array<std::uint8_t, 1u> one{0u};
    const PrefilterResult unsupported_edits =
        gatekeeper_scalar(one, one, 9u);
    REQUIRE(unsupported_edits.status == static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative));
    REQUIRE(prefilter_accepted(unsupported_edits));
    REQUIRE(unsupported_edits.lower_bound == 0u);
    REQUIRE(unsupported_edits.reserved == 0u);

    const std::vector<std::uint8_t> too_long(129u, 7u);
    const PrefilterResult unsupported_length =
        gatekeeper_scalar(too_long, too_long, 2u);
    REQUIRE(unsupported_length.status == static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative));
    REQUIRE(prefilter_accepted(unsupported_length));
    REQUIRE(unsupported_length.reserved == 0u);
}

TEST_CASE(
    "GateKeeper exact guard confirms mask rejection",
    "[gatekeeper][prefilter][guard]")
{
    const std::vector<std::uint8_t> query(32u, 0u);
    const std::vector<std::uint8_t> target(32u, 255u);
    for (const std::uint32_t max_edits : {2u, 4u}) {
        const PrefilterResult result =
            gatekeeper_scalar(query, target, max_edits);
        CAPTURE(max_edits, result.lower_bound, result.reserved);
        REQUIRE_FALSE(prefilter_accepted(result));
        REQUIRE(result.lower_bound == max_edits + 1u);
        REQUIRE(gatekeeper_diagnostic_estimate(result) == max_edits + 1u);
        REQUIRE(gatekeeper_mask_rejection_candidate(result));
        REQUIRE(gatekeeper_exact_guard_activated(result));
        REQUIRE(gatekeeper_guard_confirmed_rejection(result));
        REQUIRE_FALSE(gatekeeper_hamming_exact_rejection(result));
        check_supported_diagnostics(result, max_edits);
    }
}

TEST_CASE(
    "GateKeeper never falsely rejects exhaustive short binary pairs",
    "[gatekeeper][prefilter][exhaustive]")
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
                    const PrefilterResult result = gatekeeper_scalar(
                        query, target, max_edits);
                    CAPTURE(
                        length,
                        query,
                        target,
                        distance,
                        max_edits,
                        result.lower_bound,
                        result.reserved);
                    REQUIRE(prefilter_evaluated(result));
                    REQUIRE(result.lower_bound <= distance);
                    check_supported_diagnostics(result, max_edits);
                    REQUIRE(gatekeeper_diagnostic_estimate(result)
                            == diagnostic_estimate_oracle(
                                query, target, max_edits));
                    if (distance <= max_edits) {
                        REQUIRE(prefilter_accepted(result));
                    }
                    if (!prefilter_accepted(result)) {
                        REQUIRE(distance > max_edits);
                        REQUIRE(result.lower_bound == max_edits + 1u);
                        if (max_edits == 0u) {
                            REQUIRE(gatekeeper_hamming_exact_rejection(result));
                        } else {
                            REQUIRE(gatekeeper_guard_confirmed_rejection(result));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE(
    "GateKeeper shifted vacancies remain sound at mask boundaries",
    "[gatekeeper][prefilter][boundaries]")
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
            const PrefilterResult result = gatekeeper_scalar(
                query, *target, 2u);
            CAPTURE(length, distance, result.lower_bound, result.reserved);
            REQUIRE(distance <= 2u);
            REQUIRE(prefilter_accepted(result));
            REQUIRE(result.lower_bound == 0u);
            check_supported_diagnostics(result, 2u);
            REQUIRE(gatekeeper_diagnostic_estimate(result)
                    == diagnostic_estimate_oracle(query, *target, 2u));
        }
    }
}

TEST_CASE(
    "GateKeeper amendment crosses every 32-bit and partial-word boundary",
    "[gatekeeper][prefilter][amendment][boundaries]")
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
                const PrefilterResult result = gatekeeper_scalar(
                    query, *target, max_edits);
                const std::uint32_t expected_estimate =
                    diagnostic_estimate_oracle(query, *target, max_edits);
                CAPTURE(
                    length,
                    max_edits,
                    expected_estimate,
                    result.lower_bound,
                    result.reserved);
                REQUIRE(gatekeeper_diagnostic_estimate(result)
                        == expected_estimate);
                check_supported_diagnostics(result, max_edits);
                const std::uint32_t distance =
                    exact_levenshtein(query, *target);
                REQUIRE(result.lower_bound <= distance);
                if (distance <= max_edits) {
                    REQUIRE(prefilter_accepted(result));
                }
            }
        }
    }
}

TEST_CASE(
    "GateKeeper batch validates slices before ordered fallbacks",
    "[gatekeeper][prefilter][batch]")
{
    const std::vector<std::uint8_t> query_ranks(132u, 7u);
    const std::vector<std::uint8_t> target_ranks(132u, 7u);
    const std::array<PrefilterJob, 5u> jobs{
        PrefilterJob{
            .query_offset = (std::numeric_limits<std::uint32_t>::max)(),
            .target_offset = 0u,
            .query_length = 1u,
            .target_length = 1u,
            .max_edits = 0u,
        },
        PrefilterJob{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 129u,
            .target_length = 128u,
            .max_edits = 0u,
        },
        PrefilterJob{
            .query_offset = 132u,
            .target_offset = 132u,
            .query_length = 0u,
            .target_length = 0u,
            .max_edits = 9u,
        },
        PrefilterJob{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 129u,
            .target_length = 129u,
            .max_edits = 2u,
        },
        PrefilterJob{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 0u,
        },
    };
    const PrefilterBatchInput input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto results = gatekeeper_scalar_batch(input);
    REQUIRE(results.size() == jobs.size());

    REQUIRE(results[0].status == static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative));
    REQUIRE(prefilter_accepted(results[0]));
    REQUIRE(results[0].lower_bound == 0u);
    REQUIRE(results[0].reserved == 0u);

    REQUIRE(results[1].status == static_cast<std::uint32_t>(
        PrefilterStatus::conservative_unequal));
    REQUIRE_FALSE(prefilter_accepted(results[1]));
    REQUIRE(results[1].lower_bound == 1u);
    REQUIRE(results[1].reserved == 0u);

    REQUIRE(results[2].status == static_cast<std::uint32_t>(
        PrefilterStatus::success));
    REQUIRE(prefilter_accepted(results[2]));
    REQUIRE(results[2].reserved == 0u);

    REQUIRE(results[3].status == static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative));
    REQUIRE(prefilter_accepted(results[3]));
    REQUIRE(results[3].reserved == 0u);

    const PrefilterResult direct = gatekeeper_scalar(
        std::span<const std::uint8_t>{query_ranks}.subspan(1u, 32u),
        std::span<const std::uint8_t>{target_ranks}.subspan(1u, 32u),
        0u);
    REQUIRE(same_result(results[4], direct));
}

TEST_CASE(
    "GateKeeper hardware Vulkan matches the scalar oracle and replays fresh ranks",
    "[gatekeeper][prefilter][vulkan][hardware][differential][replay]")
{
    namespace gv = bioseq::gpu::vulkan;
    if (!vulkan_gatekeeper_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<PrefilterJob> jobs;
    const auto append_supported_job = [
        &query_ranks,
        &target_ranks,
        &jobs](
            std::uint32_t length,
            std::uint32_t max_edits,
            std::uint32_t variant) {
        // Deliberately retain non-word-aligned poison bytes around every
        // logical slice. The shader must address byte ranks from the job
        // offsets rather than accidentally consuming neighbouring records.
        query_ranks.insert(
            query_ranks.end(), 1u + variant % 3u,
            static_cast<std::uint8_t>(0xD0u + variant));
        target_ranks.insert(
            target_ranks.end(), 2u + variant % 2u,
            static_cast<std::uint8_t>(0x70u + variant));
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            query_ranks.size());
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            target_ranks.size());

        std::vector<std::uint8_t> query(length);
        std::vector<std::uint8_t> target(length);
        for (std::uint32_t position = 0u; position < length; ++position) {
            query[position] = static_cast<std::uint8_t>(
                (position * 73u + variant * 41u + length) & 0xffu);
        }
        target = query;
        const std::uint32_t scenario = variant % 4u;
        if (scenario == 1u) {
            // A known within-threshold substitution case.
            for (std::uint32_t edit = 0u; edit < max_edits; ++edit) {
                const std::uint32_t position =
                    (edit * 11u + 3u) % length;
                target[position] = static_cast<std::uint8_t>(
                    query[position] ^ 0xffu);
            }
        } else if (scenario == 2u) {
            // Force a mask candidate and exact-guard-confirmed rejection.
            for (std::uint32_t position = 0u; position < length; ++position) {
                target[position] = static_cast<std::uint8_t>(
                    query[position] ^ 0xffu);
            }
        } else if (scenario == 3u) {
            // One insertion/deletion-shaped shift remains within E=2 and E=4.
            target[0] = static_cast<std::uint8_t>(query[0] ^ 0x5au);
            std::copy_n(query.begin(), length - 1u, target.begin() + 1u);
        }

        query_ranks.insert(
            query_ranks.end(), query.begin(), query.end());
        target_ranks.insert(
            target_ranks.end(), target.begin(), target.end());
        query_ranks.push_back(static_cast<std::uint8_t>(0xE0u + variant));
        target_ranks.push_back(static_cast<std::uint8_t>(0x60u + variant));
        jobs.push_back(PrefilterJob{
            .query_offset = query_offset,
            .target_offset = target_offset,
            .query_length = length,
            .target_length = length,
            .max_edits = max_edits,
        });
    };

    for (const std::uint32_t length : {32u, 64u}) {
        for (const std::uint32_t max_edits : {2u, 4u}) {
            for (std::uint32_t variant = 0u; variant < 12u; ++variant) {
                append_supported_job(length, max_edits, variant);
            }
        }
    }

    const PrefilterBatchInput input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = gatekeeper_scalar_batch(input);
    auto& runtime = gv::Runtime::instance();
    const std::uint64_t dispatches_before =
        runtime.gatekeeper_dispatch_count();
    const auto gpu = run_gatekeeper_vulkan_batch(
        query_ranks, target_ranks, jobs);

    REQUIRE(gpu.size() == scalar.size());
    for (std::size_t index = 0u; index < scalar.size(); ++index) {
        CAPTURE(
            index,
            jobs[index].query_length,
            jobs[index].max_edits,
            scalar[index].lower_bound,
            scalar[index].reserved,
            gpu[index].lower_bound,
            gpu[index].reserved);
        CHECK(same_result(gpu[index], scalar[index]));
        check_supported_diagnostics(gpu[index], jobs[index].max_edits);
    }

    // Keep the GateKeeper replay key (shape, mode, counts, and groups)
    // identical, but replace every target slice with fresh mapped bytes.
    // A recorded command must execute again and observe the new exact matches.
    std::vector<std::uint8_t> replay_targets = target_ranks;
    for (const PrefilterJob& job : jobs) {
        std::copy_n(
            query_ranks.begin() + job.query_offset,
            job.query_length,
            replay_targets.begin() + job.target_offset);
    }
    const PrefilterBatchInput replay_input{
        .query_ranks = query_ranks,
        .target_ranks = replay_targets,
        .jobs = jobs,
    };
    const auto replay_scalar = gatekeeper_scalar_batch(replay_input);
    const auto replayed = run_gatekeeper_vulkan_batch(
        query_ranks, replay_targets, jobs);

    REQUIRE(replayed.size() == replay_scalar.size());
    bool observed_changed_result = false;
    for (std::size_t index = 0u; index < replay_scalar.size(); ++index) {
        CAPTURE(index, jobs[index].query_length, jobs[index].max_edits);
        CHECK(same_result(replayed[index], replay_scalar[index]));
        CHECK(prefilter_accepted(replayed[index]));
        CHECK(replayed[index].lower_bound == 0u);
        CHECK(gatekeeper_diagnostic_estimate(replayed[index]) == 0u);
        observed_changed_result |=
            !same_result(scalar[index], replay_scalar[index]);
    }
    CHECK(observed_changed_result);
    CHECK(runtime.gatekeeper_dispatch_count() == dispatches_before + 2u);
}

TEST_CASE(
    "GateKeeper hardware Vulkan fails open before poisoned invalid slice loads",
    "[gatekeeper][prefilter][vulkan][hardware][bounds][no-fallback]")
{
    namespace gv = bioseq::gpu::vulkan;
    if (!vulkan_gatekeeper_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    // Thirteen logical bytes allocate four packed words. Offsets ten through
    // thirteen therefore name three poison bytes plus one allocation-padding
    // byte, but are outside the logical span and must not be inspected.
    std::vector<std::uint8_t> query_ranks(13u, 0xA5u);
    std::vector<std::uint8_t> target_ranks(13u, 0x5Au);
    for (std::size_t index = 0u; index < 9u; ++index) {
        query_ranks[index] = static_cast<std::uint8_t>(index * 17u + 3u);
        target_ranks[index] = query_ranks[index];
    }
    const std::array<PrefilterJob, 6u> jobs{
        PrefilterJob{
            .query_offset = 10u,
            .target_offset = 9u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        PrefilterJob{
            .query_offset = 9u,
            .target_offset = 10u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 4u,
        },
        PrefilterJob{
            .query_offset = (std::numeric_limits<std::uint32_t>::max)(),
            .target_offset = 0u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        PrefilterJob{
            .query_offset = 13u,
            .target_offset = 0u,
            .query_length = 1u,
            .target_length = 1u,
            .max_edits = 4u,
        },
        PrefilterJob{
            .query_offset = 10u,
            .target_offset = 0u,
            .query_length = 4u,
            .target_length = 3u,
            .max_edits = 9u,
        },
        PrefilterJob{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 8u,
            .target_length = 8u,
            .max_edits = 2u,
        },
    };
    const PrefilterBatchInput input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    for (std::size_t index = 0u; index < jobs.size() - 1u; ++index) {
        REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs[index]));
    }
    REQUIRE(prefilter_job_in_bounds(input, jobs.back()));
    const auto scalar = gatekeeper_scalar_batch(input);
    auto& runtime = gv::Runtime::instance();
    const std::uint64_t dispatches_before =
        runtime.gatekeeper_dispatch_count();
    const auto gpu = run_gatekeeper_vulkan_batch(
        query_ranks, target_ranks, jobs);

    REQUIRE(gpu.size() == jobs.size());
    REQUIRE(scalar.size() == jobs.size());
    for (std::size_t index = 0u; index < jobs.size(); ++index) {
        CAPTURE(index, gpu[index].status, gpu[index].reserved);
        CHECK(same_result(gpu[index], scalar[index]));
    }
    for (std::size_t index = 0u; index < jobs.size() - 1u; ++index) {
        CHECK(gpu[index].status == static_cast<std::uint32_t>(
            PrefilterStatus::unsupported_conservative));
        CHECK(prefilter_accepted(gpu[index]));
        CHECK(gpu[index].lower_bound == 0u);
        CHECK(gpu[index].reserved == 0u);
    }
    CHECK(runtime.gatekeeper_dispatch_count() == dispatches_before + 1u);
}

TEST_CASE(
    "GateKeeper compact adapter preserves dna4 dna5 and aa27 ranks",
    "[gatekeeper][prefilter][compact][alphabet-generic]")
{
    auto dna4_query_data = "ACGTACGTACGTACGT"_dna4;
    auto dna4_target_data = "ACGTACGTTCGTACGT"_dna4;
    compact_sequence_range<std::vector<seq::dna4>> dna4_query{
        dna4_query_data};
    compact_sequence_range<std::vector<seq::dna4>> dna4_target{
        dna4_target_data};
    const PrefilterResult dna4_result = gatekeeper_scalar_compact(
        dna4_query, dna4_target, 1u);
    REQUIRE(prefilter_accepted(dna4_result));
    REQUIRE(gatekeeper_diagnostic_estimate(dna4_result) == 1u);

    auto dna5_query_data = "ACGTNACGTNACGTN"_dna5;
    auto dna5_target_data = "ACGTNACGTNNCGTN"_dna5;
    compact_sequence_range<std::vector<seq::dna5>> dna5_query{
        dna5_query_data};
    compact_sequence_range<std::vector<seq::dna5>> dna5_target{
        dna5_target_data};
    const PrefilterResult dna5_result = gatekeeper_scalar_compact(
        dna5_query, dna5_target, 1u);
    REQUIRE(prefilter_accepted(dna5_result));
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
    const PrefilterResult amino_result = gatekeeper_scalar_compact(
        amino_query, amino_target, 1u);
    REQUIRE(prefilter_accepted(amino_result));
    REQUIRE(gatekeeper_diagnostic_estimate(amino_result) == 1u);

    const auto amino_query_ranks = amino_query.to_encoded_vector();
    const auto amino_target_ranks = amino_target.to_encoded_vector();
    const PrefilterResult rank_result = gatekeeper_scalar(
        amino_query_ranks, amino_target_ranks, 1u);
    REQUIRE(same_result(amino_result, rank_result));
}

} // namespace
