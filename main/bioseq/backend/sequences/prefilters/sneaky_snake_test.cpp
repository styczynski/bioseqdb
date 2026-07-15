/**
 * @file sneaky_snake_test.cpp
 * @brief Soundness and scalar/Highway parity tests for SneakySnake.
 */
#include "common.h"
#include "common_simd.h"
#include "common_tests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

import bioseq.sequences.prefilters.sneaky_snake;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace {

[[nodiscard]] std::uint32_t exact_levenshtein(
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

[[nodiscard]] bool same_result(
    const SneakySnakeResult& lhs,
    const SneakySnakeResult& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.accepted == rhs.accepted
        && lhs.reserved == rhs.reserved;
}

[[nodiscard]] std::vector<std::vector<std::uint8_t>> binary_sequences(
    std::uint32_t max_length)
{
    std::vector<std::vector<std::uint8_t>> sequences;
    for (std::uint32_t length = 0u; length <= max_length; ++length) {
        const std::uint32_t count = 1u << length;
        for (std::uint32_t bits = 0u; bits < count; ++bits) {
            std::vector<std::uint8_t> sequence(length);
            for (std::uint32_t index = 0u; index < length; ++index) {
                sequence[index] = static_cast<std::uint8_t>(
                    (bits >> index) & 1u);
            }
            sequences.push_back(std::move(sequence));
        }
    }
    return sequences;
}

class DeterministicRanks {
public:
    [[nodiscard]] std::uint32_t next() noexcept
    {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

private:
    std::uint32_t state_ = 0x5EED1234u;
};

struct BatchFixture {
    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<SneakySnakeJob> jobs;
    std::vector<std::vector<std::uint8_t>> queries;
    std::vector<std::vector<std::uint8_t>> targets;
};

[[nodiscard]] BatchFixture random_batch(std::size_t job_count)
{
    DeterministicRanks random;
    BatchFixture fixture;
    fixture.jobs.reserve(job_count);
    fixture.queries.reserve(job_count);
    fixture.targets.reserve(job_count);

    for (std::size_t index = 0u; index < job_count; ++index) {
        const std::uint32_t query_length = random.next() % 81u;
        const bool equal_length = index % 4u != 0u;
        const std::uint32_t target_length = equal_length
            ? query_length
            : random.next() % 81u;
        const std::uint32_t alphabet_size = 2u + random.next() % 249u;

        std::vector<std::uint8_t> query(query_length);
        std::vector<std::uint8_t> target(target_length);
        for (std::uint8_t& rank : query) {
            rank = static_cast<std::uint8_t>(random.next() % alphabet_size);
        }
        for (std::uint8_t& rank : target) {
            rank = static_cast<std::uint8_t>(random.next() % alphabet_size);
        }

        // Noise around every slice verifies that offsets, rather than buffer
        // beginnings, drive both implementations.
        fixture.query_ranks.push_back(255u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            fixture.query_ranks.size());
        fixture.query_ranks.insert(
            fixture.query_ranks.end(), query.begin(), query.end());
        fixture.query_ranks.push_back(254u);

        fixture.target_ranks.push_back(253u);
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            fixture.target_ranks.size());
        fixture.target_ranks.insert(
            fixture.target_ranks.end(), target.begin(), target.end());
        fixture.target_ranks.push_back(252u);

        fixture.jobs.push_back(SneakySnakeJob{
            .query_offset = query_offset,
            .target_offset = target_offset,
            .query_length = query_length,
            .target_length = target_length,
            .max_edits = random.next() % 17u,
        });
        fixture.queries.push_back(std::move(query));
        fixture.targets.push_back(std::move(target));
    }
    return fixture;
}

TEST_CASE(
    "SneakySnake stable ABI and canonical fixtures",
    "[sneaky_snake][prefilter]")
{
    STATIC_REQUIRE(sneaky_snake_default_submaze_width == 512u);
    STATIC_REQUIRE(sneaky_snake_default_max_escape_segments == 512u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(SneakySnakeStatus::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        SneakySnakeStatus::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        SneakySnakeStatus::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(SneakySnakeJob) == 20u);
    STATIC_REQUIRE(sizeof(SneakySnakeResult) == 16u);

    const std::array<std::uint8_t, 8u> identical{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const auto identical_result = sneaky_snake_scalar(
        identical, identical, 0u);
    REQUIRE(identical_result.lower_bound == 0u);
    REQUIRE(sneaky_snake_evaluated(identical_result));
    REQUIRE(sneaky_snake_accepted(identical_result));

    auto substituted = identical;
    substituted[3] = 128u;
    const auto rejected = sneaky_snake_scalar(
        identical, substituted, 0u);
    REQUIRE(rejected.lower_bound == 1u);
    REQUIRE(sneaky_snake_evaluated(rejected));
    REQUIRE_FALSE(sneaky_snake_accepted(rejected));

    const auto accepted = sneaky_snake_scalar(
        identical, substituted, 1u);
    REQUIRE(accepted.lower_bound == 1u);
    REQUIRE(sneaky_snake_accepted(accepted));

    // Source-row parity fixtures: the original C maze compares target[n]
    // against query[n-e] and query[n+e] at global coordinates.  A one-rank
    // displacement therefore crosses one boundary obstacle and then escapes
    // along the corresponding off-diagonal row.
    const std::array<std::uint8_t, 6u> source_row{0u, 1u, 2u, 3u, 4u, 5u};
    const std::array<std::uint8_t, 6u> shifted_right{9u, 0u, 1u, 2u, 3u, 4u};
    const std::array<std::uint8_t, 6u> shifted_left{1u, 2u, 3u, 4u, 5u, 9u};
    const auto upper_diagonal = sneaky_snake_scalar(
        source_row, shifted_right, 1u);
    const auto lower_diagonal = sneaky_snake_scalar(
        source_row, shifted_left, 1u);
    REQUIRE(upper_diagonal.lower_bound == 1u);
    REQUIRE(lower_diagonal.lower_bound == 1u);
    REQUIRE(sneaky_snake_accepted(upper_diagonal));
    REQUIRE(sneaky_snake_accepted(lower_diagonal));

    const std::array<std::uint8_t, 3u> shorter{0u, 1u, 2u};
    const std::array<std::uint8_t, 6u> longer{0u, 1u, 2u, 3u, 4u, 5u};
    const auto unequal_reject = sneaky_snake_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(sneaky_snake_evaluated(unequal_reject));
    REQUIRE_FALSE(sneaky_snake_accepted(unequal_reject));
    const auto unequal_accept = sneaky_snake_scalar(shorter, longer, 3u);
    REQUIRE(unequal_accept.lower_bound == 3u);
    REQUIRE_FALSE(sneaky_snake_evaluated(unequal_accept));
    REQUIRE(sneaky_snake_accepted(unequal_accept));
}

TEST_CASE(
    "SneakySnake includes the production submaze tail and honors escape caps",
    "[sneaky_snake][prefilter][submaze]")
{
    std::vector<std::uint8_t> query(513u, 0u);
    std::vector<std::uint8_t> target(513u, 0u);
    target.front() = 1u;
    target.back() = 1u;

    const auto tail_result = sneaky_snake_scalar(query, target, 2u);
    REQUIRE(tail_result.lower_bound == 2u);
    REQUIRE(sneaky_snake_accepted(tail_result));

    query.assign(20u, 0u);
    target.assign(20u, 255u);
    const SneakySnakeOptions capped{
        .submaze_width = 8u,
        .max_escape_segments = 1u,
    };
    const auto capped_result = sneaky_snake_scalar(query, target, 10u, capped);
    REQUIRE(capped_result.lower_bound == 3u);
    REQUIRE(capped_result.lower_bound <= exact_levenshtein(query, target));
    REQUIRE(sneaky_snake_accepted(capped_result));
}

TEST_CASE(
    "SneakySnake lower bound is exhaustive-sound on short binary strings",
    "[sneaky_snake][prefilter][exhaustive]")
{
    const auto sequences = binary_sequences(6u);
    for (const auto& query : sequences) {
        for (const auto& target : sequences) {
            const std::uint32_t distance = exact_levenshtein(query, target);
            const std::uint32_t length_difference = query.size() > target.size()
                ? static_cast<std::uint32_t>(query.size() - target.size())
                : static_cast<std::uint32_t>(target.size() - query.size());
            for (std::uint32_t max_edits = 0u; max_edits <= 7u; ++max_edits) {
                const SneakySnakeResult result = sneaky_snake_scalar(
                    query, target, max_edits);
                CAPTURE(query, target, distance, max_edits, result.lower_bound);
                REQUIRE(result.lower_bound <= distance);
                REQUIRE(result.reserved == 0u);
                if (query.size() == target.size()) {
                    REQUIRE(sneaky_snake_evaluated(result));
                    REQUIRE(sneaky_snake_accepted(result)
                            == (result.lower_bound <= max_edits));
                } else {
                    REQUIRE_FALSE(sneaky_snake_evaluated(result));
                    REQUIRE(result.lower_bound == length_difference);
                    REQUIRE(sneaky_snake_accepted(result)
                            == (length_difference <= max_edits));
                }
                if (distance <= max_edits) {
                    REQUIRE(sneaky_snake_accepted(result));
                }
            }
        }
    }
}

TEST_CASE(
    "SneakySnake Highway batch exactly matches scalar for arbitrary ranks",
    "[sneaky_snake][prefilter][simd]")
{
    const hn::ScalableTag<std::uint32_t> d;
    const std::size_t job_count = std::max<std::size_t>(
        97u,
        hn::Lanes(d) * 3u + 5u);
    BatchFixture fixture = random_batch(job_count);
    const SneakySnakeBatchInput input{
        .query_ranks = fixture.query_ranks,
        .target_ranks = fixture.target_ranks,
        .jobs = fixture.jobs,
    };

    const std::array<SneakySnakeOptions, 4u> option_sets{
        SneakySnakeOptions{},
        SneakySnakeOptions{
            .submaze_width = 7u,
            .max_escape_segments = 7u,
        },
        SneakySnakeOptions{
            .submaze_width = 13u,
            .max_escape_segments = 2u,
        },
        SneakySnakeOptions{
            .submaze_width = 0u,
            .max_escape_segments = 0u,
        },
    };

    for (const SneakySnakeOptions options : option_sets) {
        const auto scalar = sneaky_snake_scalar_batch(input, options);
        const auto simd = sneaky_snake_simd_batch_for(d, input, options);
        const auto active_target = sneaky_snake_simd_batch(input, options);
        REQUIRE(scalar.size() == fixture.jobs.size());
        REQUIRE(simd.size() == scalar.size());
        REQUIRE(active_target.size() == scalar.size());

        for (std::size_t index = 0u; index < scalar.size(); ++index) {
            const std::uint32_t distance = exact_levenshtein(
                fixture.queries[index], fixture.targets[index]);
            CAPTURE(
                index,
                options.submaze_width,
                options.max_escape_segments,
                distance,
                scalar[index].lower_bound,
                simd[index].lower_bound);
            REQUIRE(same_result(simd[index], scalar[index]));
            REQUIRE(same_result(active_target[index], scalar[index]));
            REQUIRE(scalar[index].lower_bound <= distance);
            if (distance <= fixture.jobs[index].max_edits) {
                REQUIRE(sneaky_snake_accepted(scalar[index]));
            }
        }
    }
}

TEST_CASE(
    "SneakySnake malformed batch slices fail open without reading buffers",
    "[sneaky_snake][prefilter][bounds]")
{
    const std::array<std::uint8_t, 2u> query{1u, 2u};
    const std::array<std::uint8_t, 2u> target{1u, 2u};
    const std::array<SneakySnakeJob, 1u> jobs{SneakySnakeJob{
        .query_offset = 2u,
        .target_offset = 0u,
        .query_length = 1u,
        .target_length = 1u,
        .max_edits = 0u,
    }};
    const SneakySnakeBatchInput input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };

    REQUIRE_FALSE(sneaky_snake_job_in_bounds(input, jobs.front()));
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = sneaky_snake_simd_batch(input);
    REQUIRE(scalar.size() == 1u);
    REQUIRE(same_result(scalar.front(), simd.front()));
    REQUIRE(scalar.front().status == static_cast<std::uint32_t>(
        SneakySnakeStatus::unsupported_conservative));
    REQUIRE(sneaky_snake_accepted(scalar.front()));
}

} // namespace
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
