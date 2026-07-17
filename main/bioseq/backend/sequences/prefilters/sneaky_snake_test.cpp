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
import bioseq.sequences.prefilters.sneaky_snake_vulkan;
import bioseq.gpu.vulkan.phase_b_abi;
import bioseq.gpu.vulkan.phase_c_abi;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
using namespace bioseq::sequences::prefilters;

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
    const PrefilterResult& lhs,
    const PrefilterResult& rhs) noexcept
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
    std::vector<PrefilterJob> jobs;
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

    constexpr std::array<std::uint32_t, 4u> boundary_lengths{
        511u, 512u, 513u, 1025u};

    for (std::size_t index = 0u; index < job_count; ++index) {
        const std::uint32_t query_length = index < boundary_lengths.size()
            ? boundary_lengths[index]
            : random.next() % 81u;
        const bool equal_length = index < boundary_lengths.size()
            || index % 4u != 0u;
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

        fixture.jobs.push_back(PrefilterJob{
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
    STATIC_REQUIRE(static_cast<std::uint32_t>(PrefilterStatus::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        PrefilterStatus::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(PrefilterJob) == 20u);
    STATIC_REQUIRE(sizeof(PrefilterResult) == 16u);

    const std::array<std::uint8_t, 8u> identical{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const auto identical_result = sneaky_snake_scalar(
        identical, identical, 0u);
    REQUIRE(identical_result.lower_bound == 0u);
    REQUIRE(prefilter_evaluated(identical_result));
    REQUIRE(prefilter_accepted(identical_result));

    auto substituted = identical;
    substituted[3] = 128u;
    const auto rejected = sneaky_snake_scalar(
        identical, substituted, 0u);
    REQUIRE(rejected.lower_bound == 1u);
    REQUIRE(prefilter_evaluated(rejected));
    REQUIRE_FALSE(prefilter_accepted(rejected));

    const auto accepted = sneaky_snake_scalar(
        identical, substituted, 1u);
    REQUIRE(accepted.lower_bound == 1u);
    REQUIRE(prefilter_accepted(accepted));

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
    REQUIRE(prefilter_accepted(upper_diagonal));
    REQUIRE(prefilter_accepted(lower_diagonal));

    const std::array<std::uint8_t, 3u> shorter{0u, 1u, 2u};
    const std::array<std::uint8_t, 6u> longer{0u, 1u, 2u, 3u, 4u, 5u};
    const auto unequal_reject = sneaky_snake_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(prefilter_evaluated(unequal_reject));
    REQUIRE_FALSE(prefilter_accepted(unequal_reject));
    const auto unequal_accept = sneaky_snake_scalar(shorter, longer, 3u);
    REQUIRE(unequal_accept.lower_bound == 3u);
    REQUIRE_FALSE(prefilter_evaluated(unequal_accept));
    REQUIRE(prefilter_accepted(unequal_accept));
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
    REQUIRE(prefilter_accepted(tail_result));

    query.assign(20u, 0u);
    target.assign(20u, 255u);
    const SneakySnakeOptions capped{
        .submaze_width = 8u,
        .max_escape_segments = 1u,
    };
    const auto capped_result = sneaky_snake_scalar(query, target, 10u, capped);
    REQUIRE(capped_result.lower_bound == 3u);
    REQUIRE(capped_result.lower_bound <= exact_levenshtein(query, target));
    REQUIRE(prefilter_accepted(capped_result));
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
                const PrefilterResult result = sneaky_snake_scalar(
                    query, target, max_edits);
                CAPTURE(query, target, distance, max_edits, result.lower_bound);
                REQUIRE(result.lower_bound <= distance);
                REQUIRE(result.reserved == 0u);
                if (query.size() == target.size()) {
                    REQUIRE(prefilter_evaluated(result));
                    REQUIRE(prefilter_accepted(result)
                            == (result.lower_bound <= max_edits));
                } else {
                    REQUIRE_FALSE(prefilter_evaluated(result));
                    REQUIRE(result.lower_bound == length_difference);
                    REQUIRE(prefilter_accepted(result)
                            == (length_difference <= max_edits));
                }
                if (distance <= max_edits) {
                    REQUIRE(prefilter_accepted(result));
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
    const PrefilterBatchInput input{
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
                REQUIRE(prefilter_accepted(scalar[index]));
            }
        }
    }
}

TEST_CASE(
    "SneakySnake Highway scans full vectors and exact mismatch boundaries",
    "[sneaky_snake][prefilter][simd]")
{
    const hn::ScalableTag<std::uint32_t> d;
    const hn::ScalableTag<std::uint8_t> byte_d;
    const std::uint32_t lanes = static_cast<std::uint32_t>(
        hn::Lanes(byte_d));
    const std::uint32_t length = 3u * lanes + 5u;
    std::vector<std::uint32_t> mismatch_positions{
        1u,
        lanes - 1u,
        lanes,
        lanes + 1u,
        2u * lanes - 1u,
        2u * lanes,
        2u * lanes + 1u,
        length - 1u,
        length,
    };
    std::sort(mismatch_positions.begin(), mismatch_positions.end());
    mismatch_positions.erase(
        std::unique(mismatch_positions.begin(), mismatch_positions.end()),
        mismatch_positions.end());

    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<PrefilterJob> jobs;
    for (const std::uint32_t mismatch : mismatch_positions) {
        query_ranks.push_back(17u);
        target_ranks.push_back(19u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            query_ranks.size());
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            target_ranks.size());
        query_ranks.insert(query_ranks.end(), length, 255u);
        target_ranks.insert(target_ranks.end(), length, 255u);
        if (mismatch < length) {
            target_ranks[static_cast<std::size_t>(target_offset) + mismatch]
                = 0u;
        }
        query_ranks.push_back(23u);
        target_ranks.push_back(29u);
        jobs.push_back(PrefilterJob{
            .query_offset = query_offset,
            .target_offset = target_offset,
            .query_length = length,
            .target_length = length,
            .max_edits = 0u,
        });
    }

    const PrefilterBatchInput input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = sneaky_snake_simd_batch_for(d, input);
    const auto active_target = sneaky_snake_simd_batch(input);
    REQUIRE(simd.size() == scalar.size());
    REQUIRE(active_target.size() == scalar.size());
    for (std::size_t index = 0u; index < scalar.size(); ++index) {
        CAPTURE(index, mismatch_positions[index], lanes, length);
        REQUIRE(same_result(simd[index], scalar[index]));
        REQUIRE(same_result(active_target[index], scalar[index]));
        if (mismatch_positions[index] < length) {
            REQUIRE(scalar[index].lower_bound == 1u);
            REQUIRE_FALSE(prefilter_accepted(scalar[index]));
        } else {
            REQUIRE(scalar[index].lower_bound == 0u);
            REQUIRE(prefilter_accepted(scalar[index]));
        }
    }
}

TEST_CASE(
    "SneakySnake Highway candidate mask preserves the exact longest diagonal",
    "[sneaky_snake][prefilter][simd]")
{
    const hn::ScalableTag<std::uint32_t> d;
    const hn::ScalableTag<std::uint8_t> byte_d;
    const std::uint32_t lanes = static_cast<std::uint32_t>(
        hn::Lanes(byte_d));
    const std::uint32_t length = 8u * lanes + 64u;
    const std::uint32_t position = 3u * lanes + 10u;
    const std::uint32_t max_edits = 2u * lanes + 3u;
    const std::uint32_t lower = position - max_edits;
    const std::uint32_t winning_start = position + lanes;
    const std::uint32_t expected_run = length - winning_start;
    constexpr std::uint32_t main_run = 3u;

    std::vector<std::uint8_t> query(length, 255u);
    std::vector<std::uint8_t> target(length, 254u);
    for (std::uint32_t index = position; index < length; ++index) {
        target[index] = static_cast<std::uint8_t>(
            ((index - position) * 73u + 11u) % 251u);
    }

    const std::uint8_t filter_rank = target[position + main_run];
    query[lower + main_run] = filter_rank;
    query[lower + lanes - 1u + main_run] = filter_rank;
    query[lower + lanes + main_run] = filter_rank;
    for (std::uint32_t index = 0u; index < expected_run; ++index) {
        query[winning_start + index] = target[position + index];
    }
    for (std::uint32_t index = 0u; index < main_run; ++index) {
        query[position + index] = target[position + index];
    }
    query[position + main_run] = 252u;

    const std::uint32_t scalar_best = sneaky_snake_detail::
        longest_diagonal_run_for<sneaky_snake_detail::CpuTraversal::scalar>(
            nullptr,
            query,
            target,
            position,
            length,
            max_edits);
    const std::uint32_t simd_best = sneaky_snake_detail::
        longest_diagonal_run_for<sneaky_snake_detail::CpuTraversal::highway>(
            d,
            query,
            target,
            position,
            length,
            max_edits);
    REQUIRE(scalar_best == expected_run);
    REQUIRE(simd_best == expected_run);
}

TEST_CASE(
    "SneakySnake hardware Vulkan batch matches scalar across submaze boundaries",
    "[sneaky_snake][prefilter][vulkan][hardware]")
{
    namespace vp = bioseq::sequences::prefilters;
    namespace gv = bioseq::gpu::vulkan;
    if (!vp::vulkan_sneaky_snake_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    const auto require_gpu_parity = [](const BatchFixture& fixture) {
        const PrefilterBatchInput input{
            .query_ranks = fixture.query_ranks,
            .target_ranks = fixture.target_ranks,
            .jobs = fixture.jobs,
        };
        const auto scalar = sneaky_snake_scalar_batch(input);
        const auto gpu = vp::run_sneaky_snake_vulkan_batch(
            fixture.query_ranks, fixture.target_ranks, fixture.jobs);

        // Keep the recorded-command key identical while changing mapped input
        // bytes. Equal supported jobs become exact matches, which proves that
        // replay executes and observes fresh host writes rather than returning
        // the first submission's result buffer.
        std::vector<std::uint8_t> replay_targets = fixture.target_ranks;
        for (const PrefilterJob& job : fixture.jobs) {
            if (job.query_length == job.target_length
                && job.query_length != 0u
                && job.max_edits <= gv::k_gpu_sneaky_snake_max_edits) {
                std::copy_n(
                    fixture.query_ranks.begin() + job.query_offset,
                    job.query_length,
                    replay_targets.begin() + job.target_offset);
            }
        }
        const PrefilterBatchInput replay_input{
            .query_ranks = fixture.query_ranks,
            .target_ranks = replay_targets,
            .jobs = fixture.jobs,
        };
        const auto replay_scalar = sneaky_snake_scalar_batch(replay_input);
        const auto replayed = vp::run_sneaky_snake_vulkan_batch(
            fixture.query_ranks, replay_targets, fixture.jobs);
        REQUIRE(gpu.size() == scalar.size());
        REQUIRE(replayed.size() == gpu.size());
        bool observed_changed_result = false;
        for (std::size_t index = 0u; index < scalar.size(); ++index) {
            CAPTURE(index, fixture.jobs[index].query_length,
                    fixture.jobs[index].target_length,
                    fixture.jobs[index].max_edits);
            if (fixture.jobs[index].max_edits
                > gv::k_gpu_sneaky_snake_max_edits) {
                CHECK(gpu[index].lower_bound == 0u);
                CHECK(gpu[index].status
                    == static_cast<std::uint32_t>(
                        PrefilterStatus::unsupported_conservative));
                CHECK(gpu[index].accepted == 1u);
                CHECK(gpu[index].reserved == 0u);
                CHECK(replayed[index].lower_bound == 0u);
                CHECK(replayed[index].status
                    == static_cast<std::uint32_t>(
                        PrefilterStatus::unsupported_conservative));
                CHECK(replayed[index].accepted == 1u);
                CHECK(replayed[index].reserved == 0u);
                continue;
            }
            CHECK(gpu[index].lower_bound == scalar[index].lower_bound);
            CHECK(gpu[index].status == scalar[index].status);
            CHECK(gpu[index].accepted == scalar[index].accepted);
            CHECK(gpu[index].reserved == scalar[index].reserved);
            CHECK(replayed[index].lower_bound
                == replay_scalar[index].lower_bound);
            CHECK(replayed[index].status == replay_scalar[index].status);
            CHECK(replayed[index].accepted == replay_scalar[index].accepted);
            CHECK(replayed[index].reserved == replay_scalar[index].reserved);
            observed_changed_result |=
                replay_scalar[index].lower_bound != scalar[index].lower_bound
                || replay_scalar[index].status != scalar[index].status
                || replay_scalar[index].accepted != scalar[index].accepted;
        }
        CHECK(observed_changed_result);
    };

    // Force the independent-invocation path while retaining offsets, arbitrary
    // uint8 ranks, partial workgroups, and all submaze boundary lengths.
    BatchFixture low_edit_fixture = random_batch(12u);
    for (PrefilterJob& job : low_edit_fixture.jobs) {
        job.max_edits = std::min(job.max_edits, 8u);
    }
    require_gpu_parity(low_edit_fixture);

    BatchFixture high_edit_fixture;
    const auto append_job = [&high_edit_fixture](
        std::span<const std::uint8_t> query,
        std::span<const std::uint8_t> target,
        std::uint32_t max_edits) {
        high_edit_fixture.query_ranks.push_back(255u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            high_edit_fixture.query_ranks.size());
        high_edit_fixture.query_ranks.insert(
            high_edit_fixture.query_ranks.end(), query.begin(), query.end());
        high_edit_fixture.query_ranks.push_back(254u);

        high_edit_fixture.target_ranks.push_back(253u);
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            high_edit_fixture.target_ranks.size());
        high_edit_fixture.target_ranks.insert(
            high_edit_fixture.target_ranks.end(), target.begin(), target.end());
        high_edit_fixture.target_ranks.push_back(252u);

        high_edit_fixture.jobs.push_back(PrefilterJob{
            .query_offset = query_offset,
            .target_offset = target_offset,
            .query_length = static_cast<std::uint32_t>(query.size()),
            .target_length = static_cast<std::uint32_t>(target.size()),
            .max_edits = max_edits,
        });
    };

    std::vector<std::uint8_t> shifted_query(600u);
    std::vector<std::uint8_t> shifted_target(600u, 251u);
    for (std::size_t index = 0u; index < shifted_query.size(); ++index) {
        shifted_query[index] = static_cast<std::uint8_t>(
            (index * 37u + index / 7u * 11u) % 251u);
    }
    constexpr std::size_t kWinningShift = 17u;
    for (std::size_t index = kWinningShift;
         index < shifted_target.size(); ++index) {
        shifted_target[index] = shifted_query[index - kWinningShift];
    }
    append_job(shifted_query, shifted_target, 31u);
    append_job(shifted_query, shifted_target, 32u);
    append_job({}, {}, 31u);
    const std::array<std::uint8_t, 1u> one_rank{7u};
    append_job({}, one_rank, 31u);
    require_gpu_parity(high_edit_fixture);
}

TEST_CASE(
    "SneakySnake malformed batch slices fail open without reading buffers",
    "[sneaky_snake][prefilter][bounds]")
{
    const std::array<std::uint8_t, 2u> query{1u, 2u};
    const std::array<std::uint8_t, 2u> target{1u, 2u};
    const std::array<PrefilterJob, 1u> jobs{PrefilterJob{
        .query_offset = 2u,
        .target_offset = 0u,
        .query_length = 1u,
        .target_length = 1u,
        .max_edits = 0u,
    }};
    const PrefilterBatchInput input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };

    REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs.front()));
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = sneaky_snake_simd_batch(input);
    REQUIRE(scalar.size() == 1u);
    REQUIRE(same_result(scalar.front(), simd.front()));
    REQUIRE(scalar.front().status == static_cast<std::uint32_t>(
        PrefilterStatus::unsupported_conservative));
    REQUIRE(prefilter_accepted(scalar.front()));
}

} // namespace
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
