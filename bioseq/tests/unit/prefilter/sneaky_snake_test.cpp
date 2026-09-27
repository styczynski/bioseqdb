/**
 * @file sneaky_snake_test.cpp
 * @brief Check SneakySnake lower bounds and agreement between scalar, Highway, and Vulkan results.
 *
 * Independent edit-distance tests check that valid matches survive. Other
 * cases cover chunk boundaries, inactive SIMD lanes, invalid slices, and
 * Vulkan command replay after input bytes change.
 */
#include <hwy/highway.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.gpu.vulkan.abi;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
using namespace bioseq::prefilter;

namespace {

/**
 * @brief Get the SneakySnake kernel for this test's Highway target.
 * @return The generated table's callable.
 * @throws std::logic_error If the table has no SneakySnake kernel.
 */
[[nodiscard]] ::bioseq::simd_dispatch::sneaky_snake_simd_batch_kernel
fixed_sneaky_snake_kernel()
{
    const auto kernel = ::bioseq::simd_dispatch::highway_test_kernel_table()
        .common.sneaky_snake_simd_batch;
    if (kernel == nullptr) {
        throw std::logic_error{
            "generated fixed-target test table lacks SneakySnake"};
    }
    return kernel;
}

/**
 * @brief Compute exact edit distance independently for the filter tests.
 *
 * Insertions, deletions, and substitutions cost one. The calculation keeps
 * the current and previous rows of the dynamic-programming matrix.
 * @param[in] query Query ranks in sequence order.
 * @param[in] target Target ranks in sequence order.
 * @return Global Levenshtein distance.
 */
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

/**
 * @brief Compare the lower bound, status, acceptance flag, and diagnostics.
 * @return True when all four fields match.
 */
[[nodiscard]] bool same_result(
    const prefilter_result& lhs,
    const prefilter_result& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.is_accepted == rhs.is_accepted
        && lhs.diagnostics == rhs.diagnostics;
}

/**
 * @brief Generate every binary sequence up to max_length, including the empty sequence.
 * @return Sequences ordered by length, then by integer bit pattern with the lowest bit first.
 */
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

/** Reproducible rank generator for test inputs. */
class deterministic_ranks {
public:
    /** Advance the generator and return the next 32-bit value. */
    [[nodiscard]] std::uint32_t next() noexcept
    {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

private:
    std::uint32_t state_ = 0x5EED1234u;
};

/**
 * @brief Keep both batch buffers and separate sequence copies for comparisons.
 *
 * Jobs address slices in query_ranks and target_ranks. The separate queries
 * and targets let the exact-distance reference check each pair.
 */
struct batch_fixture {
    /** Query bytes with extra ranks around each slice to expose incorrect offsets. */
    std::vector<std::uint8_t> query_ranks;
    /** Target bytes with extra ranks around each slice to expose incorrect offsets. */
    std::vector<std::uint8_t> target_ranks;
    /** Slice offsets, lengths, and edit limits for each query-target pair. */
    std::vector<prefilter_job> jobs;
    /** Separate query sequences for exact-distance checks. */
    std::vector<std::vector<std::uint8_t>> queries;
    /** Separate target sequences for exact-distance checks. */
    std::vector<std::vector<std::uint8_t>> targets;
};

/**
 * @brief Build a reproducible batch with varied lengths, alphabets, and edit limits.
 *
 * The first cases cross the 512-base chunk boundary. Extra bytes surround each
 * slice to catch reads outside it.
 * @param[in] job_count Number of query-target pairs.
 * @return Buffers, jobs, and separate copies of each sequence pair.
 */
[[nodiscard]] batch_fixture random_batch(std::size_t job_count)
{
    deterministic_ranks random;
    batch_fixture fixture;
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

        // Extra ranks around each slice make incorrect offsets visible in the result.
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

        fixture.jobs.push_back(prefilter_job{
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
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    STATIC_REQUIRE(k_sneaky_snake_default_submaze_width == 512u);
    STATIC_REQUIRE(k_sneaky_snake_default_max_escape_segments == 512u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(prefilter_status::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(prefilter_job) == 20u);
    STATIC_REQUIRE(sizeof(prefilter_result) == 16u);

    const std::array<std::uint8_t, 8u> identical{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const auto identical_result = sneaky_snake_scalar(
        identical, identical, 0u);
    REQUIRE(identical_result.lower_bound == 0u);
    REQUIRE(is_prefilter_evaluated(identical_result));
    REQUIRE(is_prefilter_accepted(identical_result));

    auto substituted = identical;
    substituted[3] = 128u;
    const auto rejected = sneaky_snake_scalar(
        identical, substituted, 0u);
    REQUIRE(rejected.lower_bound == 1u);
    REQUIRE(is_prefilter_evaluated(rejected));
    REQUIRE_FALSE(is_prefilter_accepted(rejected));

    const auto accepted = sneaky_snake_scalar(
        identical, substituted, 1u);
    REQUIRE(accepted.lower_bound == 1u);
    REQUIRE(is_prefilter_accepted(accepted));

    // A one-rank shift reaches a matching off-diagonal after one obstacle.
    // These cases check both shift directions and expect a lower bound of one.
    const std::array<std::uint8_t, 6u> source_row{0u, 1u, 2u, 3u, 4u, 5u};
    const std::array<std::uint8_t, 6u> shifted_right{9u, 0u, 1u, 2u, 3u, 4u};
    const std::array<std::uint8_t, 6u> shifted_left{1u, 2u, 3u, 4u, 5u, 9u};
    const auto upper_diagonal = sneaky_snake_scalar(
        source_row, shifted_right, 1u);
    const auto lower_diagonal = sneaky_snake_scalar(
        source_row, shifted_left, 1u);
    REQUIRE(upper_diagonal.lower_bound == 1u);
    REQUIRE(lower_diagonal.lower_bound == 1u);
    REQUIRE(is_prefilter_accepted(upper_diagonal));
    REQUIRE(is_prefilter_accepted(lower_diagonal));

    const std::array<std::uint8_t, 3u> shorter{0u, 1u, 2u};
    const std::array<std::uint8_t, 6u> longer{0u, 1u, 2u, 3u, 4u, 5u};
    const auto unequal_reject = sneaky_snake_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(is_prefilter_evaluated(unequal_reject));
    REQUIRE_FALSE(is_prefilter_accepted(unequal_reject));
    const auto unequal_accept = sneaky_snake_scalar(shorter, longer, 3u);
    REQUIRE(unequal_accept.lower_bound == 3u);
    REQUIRE_FALSE(is_prefilter_evaluated(unequal_accept));
    REQUIRE(is_prefilter_accepted(unequal_accept));
}

TEST_CASE(
    "SneakySnake includes the production submaze tail and honors escape caps",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    std::vector<std::uint8_t> query(513u, 0u);
    std::vector<std::uint8_t> target(513u, 0u);
    target.front() = 1u;
    target.back() = 1u;

    const auto tail_result = sneaky_snake_scalar(query, target, 2u);
    REQUIRE(tail_result.lower_bound == 2u);
    REQUIRE(is_prefilter_accepted(tail_result));

    query.assign(20u, 0u);
    target.assign(20u, 255u);
    const sneaky_snake_options capped{
        .submaze_width = 8u,
        .max_escape_segments = 1u,
    };
    const auto capped_result = sneaky_snake_scalar(query, target, 10u, capped);
    REQUIRE(capped_result.lower_bound == 3u);
    REQUIRE(capped_result.lower_bound <= exact_levenshtein(query, target));
    REQUIRE(is_prefilter_accepted(capped_result));
}

TEST_CASE(
    "SneakySnake lower bound is exhaustive-sound on short binary strings",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const auto sequences = binary_sequences(6u);
    for (const auto& query : sequences) {
        for (const auto& target : sequences) {
            const std::uint32_t distance = exact_levenshtein(query, target);
            const std::uint32_t length_difference = query.size() > target.size()
                ? static_cast<std::uint32_t>(query.size() - target.size())
                : static_cast<std::uint32_t>(target.size() - query.size());
            for (std::uint32_t max_edits = 0u; max_edits <= 7u; ++max_edits) {
                const prefilter_result result = sneaky_snake_scalar(
                    query, target, max_edits);
                CAPTURE(query, target, distance, max_edits, result.lower_bound);
                REQUIRE(result.lower_bound <= distance);
                REQUIRE(result.diagnostics == 0u);
                if (query.size() == target.size()) {
                    REQUIRE(is_prefilter_evaluated(result));
                    REQUIRE(is_prefilter_accepted(result)
                            == (result.lower_bound <= max_edits));
                } else {
                    REQUIRE_FALSE(is_prefilter_evaluated(result));
                    REQUIRE(result.lower_bound == length_difference);
                    REQUIRE(is_prefilter_accepted(result)
                            == (length_difference <= max_edits));
                }
                if (distance <= max_edits) {
                    REQUIRE(is_prefilter_accepted(result));
                }
            }
        }
    }
}

TEST_CASE(
    "SneakySnake Highway batch exactly matches scalar for arbitrary ranks",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
    const hn::ScalableTag<std::uint32_t> d;
    const std::size_t job_count = std::max<std::size_t>(
        97u,
        hn::Lanes(d) * 3u + 5u);
    batch_fixture fixture = random_batch(job_count);
    const prefilter_batch_input input{
        .query_ranks = fixture.query_ranks,
        .target_ranks = fixture.target_ranks,
        .jobs = fixture.jobs,
    };

    const std::array<sneaky_snake_options, 4u> option_sets{
        sneaky_snake_options{},
        sneaky_snake_options{
            .submaze_width = 7u,
            .max_escape_segments = 7u,
        },
        sneaky_snake_options{
            .submaze_width = 13u,
            .max_escape_segments = 2u,
        },
        sneaky_snake_options{
            .submaze_width = 0u,
            .max_escape_segments = 0u,
        },
    };

    for (const sneaky_snake_options options : option_sets) {
        const auto scalar = sneaky_snake_scalar_batch(input, options);
        const auto simd = fixed_sneaky_snake_kernel()(input, options);
        REQUIRE(scalar.size() == fixture.jobs.size());
        REQUIRE(simd.size() == scalar.size());

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
            REQUIRE(scalar[index].lower_bound <= distance);
            if (distance <= fixture.jobs[index].max_edits) {
                REQUIRE(is_prefilter_accepted(scalar[index]));
            }
        }
    }
}

TEST_CASE(
    "SneakySnake Highway scans full vectors and exact mismatch boundaries",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
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
    std::vector<prefilter_job> jobs;
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
        jobs.push_back(prefilter_job{
            .query_offset = query_offset,
            .target_offset = target_offset,
            .query_length = length,
            .target_length = length,
            .max_edits = 0u,
        });
    }

    const prefilter_batch_input input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = fixed_sneaky_snake_kernel()(input, {});
    REQUIRE(simd.size() == scalar.size());
    for (std::size_t index = 0u; index < scalar.size(); ++index) {
        CAPTURE(index, mismatch_positions[index], lanes, length);
        REQUIRE(same_result(simd[index], scalar[index]));
        if (mismatch_positions[index] < length) {
            REQUIRE(scalar[index].lower_bound == 1u);
            REQUIRE_FALSE(is_prefilter_accepted(scalar[index]));
        } else {
            REQUIRE(scalar[index].lower_bound == 0u);
            REQUIRE(is_prefilter_accepted(scalar[index]));
        }
    }
}

TEST_CASE(
    "SneakySnake generated fixed-target candidate mask preserves the longest diagonal",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
    constexpr std::uint32_t submaze_width = 512u;
    constexpr std::uint32_t length = 2u * submaze_width;
    constexpr std::uint32_t position = submaze_width;
    constexpr std::uint32_t candidate_block = 64u;
    constexpr std::uint32_t max_edits = 4u * candidate_block + 3u;
    constexpr std::uint32_t lower = position - max_edits;
    constexpr std::uint32_t winning_start = position - candidate_block;

    std::vector<std::uint8_t> target(length, 1u);
    target[lower] = 0u;
    target[lower + candidate_block - 1u] = 0u;
    target[lower + candidate_block] = 0u;
    target[winning_start] = 0u;
    target[winning_start + 1u] = 2u;

    // Keep the first chunk equal. In the second chunk, place the longest
    // matching diagonal past three 64-position candidate blocks to check that
    // the SIMD search reaches it.
    std::copy_n(
        target.begin() + winning_start,
        candidate_block,
        target.begin() + position);
    for (std::uint32_t index = position + candidate_block;
         index < length;
         ++index) {
        target[index] = static_cast<std::uint8_t>(
            3u + index % 251u);
    }

    std::vector<std::uint8_t> query = target;
    std::copy_n(
        target.begin() + position,
        submaze_width,
        query.begin() + winning_start);

    const std::array<prefilter_job, 1u> jobs{prefilter_job{
        .query_offset = 0u,
        .target_offset = 0u,
        .query_length = length,
        .target_length = length,
        .max_edits = max_edits,
    }};
    const prefilter_batch_input input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };
    const sneaky_snake_options options{
        .submaze_width = submaze_width,
        .max_escape_segments = submaze_width,
    };

    const auto scalar = sneaky_snake_scalar_batch(input, options);
    const auto fixed = fixed_sneaky_snake_kernel()(input, options);
    REQUIRE(scalar.size() == 1u);
    REQUIRE(fixed.size() == scalar.size());
    REQUIRE(scalar.front().lower_bound == 0u);
    REQUIRE(is_prefilter_accepted(scalar.front()));
    REQUIRE(same_result(fixed.front(), scalar.front()));
}

TEST_CASE(
    "SneakySnake hardware Vulkan batch matches scalar across submaze boundaries",
    "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]")
{
    namespace vp = bioseq::prefilter;
    namespace gv = bioseq::gpu::vulkan;
    if (!vp::is_vulkan_sneaky_snake_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    const auto require_gpu_parity = [](const batch_fixture& fixture) {
        const prefilter_batch_input input{
            .query_ranks = fixture.query_ranks,
            .target_ranks = fixture.target_ranks,
            .jobs = fixture.jobs,
        };
        const auto scalar = sneaky_snake_scalar_batch(input);
        const auto gpu = vp::run_sneaky_snake_vulkan_batch(
            fixture.query_ranks, fixture.target_ranks, fixture.jobs);

        // Reuse the command shape with new target bytes. Supported equal-length
        // pairs become exact matches, so changed results confirm that replay reads
        // the latest host writes.
        std::vector<std::uint8_t> replay_targets = fixture.target_ranks;
        for (const prefilter_job& job : fixture.jobs) {
            if (job.query_length == job.target_length
                && job.query_length != 0u
                && job.max_edits <= gv::k_gpu_sneaky_snake_max_edits) {
                std::copy_n(
                    fixture.query_ranks.begin() + job.query_offset,
                    job.query_length,
                    replay_targets.begin() + job.target_offset);
            }
        }
        const prefilter_batch_input replay_input{
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
                        prefilter_status::unsupported_conservative));
                CHECK(gpu[index].is_accepted == 1u);
                CHECK(gpu[index].diagnostics == 0u);
                CHECK(replayed[index].lower_bound == 0u);
                CHECK(replayed[index].status
                    == static_cast<std::uint32_t>(
                        prefilter_status::unsupported_conservative));
                CHECK(replayed[index].is_accepted == 1u);
                CHECK(replayed[index].diagnostics == 0u);
                continue;
            }
            CHECK(gpu[index].lower_bound == scalar[index].lower_bound);
            CHECK(gpu[index].status == scalar[index].status);
            CHECK(gpu[index].is_accepted == scalar[index].is_accepted);
            CHECK(gpu[index].diagnostics == scalar[index].diagnostics);
            CHECK(replayed[index].lower_bound
                == replay_scalar[index].lower_bound);
            CHECK(replayed[index].status == replay_scalar[index].status);
            CHECK(replayed[index].is_accepted == replay_scalar[index].is_accepted);
            CHECK(replayed[index].diagnostics == replay_scalar[index].diagnostics);
            observed_changed_result |=
                replay_scalar[index].lower_bound != scalar[index].lower_bound
                || replay_scalar[index].status != scalar[index].status
                || replay_scalar[index].is_accepted != scalar[index].is_accepted;
        }
        CHECK(observed_changed_result);
    };

    // Small edit limits select one GPU invocation per job. Check slice
    // offsets and chunk boundaries with a partially filled workgroup.
    batch_fixture low_edit_fixture = random_batch(12u);
    for (prefilter_job& job : low_edit_fixture.jobs) {
        job.max_edits = std::min(job.max_edits, 8u);
    }
    require_gpu_parity(low_edit_fixture);

    batch_fixture high_edit_fixture;
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

        high_edit_fixture.jobs.push_back(prefilter_job{
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
    constexpr std::size_t k_winning_shift = 17u;
    for (std::size_t index = k_winning_shift;
         index < shifted_target.size(); ++index) {
        shifted_target[index] = shifted_query[index - k_winning_shift];
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
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const std::array<std::uint8_t, 2u> query{1u, 2u};
    const std::array<std::uint8_t, 2u> target{1u, 2u};
    const std::array<prefilter_job, 1u> jobs{prefilter_job{
        .query_offset = 2u,
        .target_offset = 0u,
        .query_length = 1u,
        .target_length = 1u,
        .max_edits = 0u,
    }};
    const prefilter_batch_input input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };

    REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs.front()));
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = fixed_sneaky_snake_kernel()(input, {});
    REQUIRE(scalar.size() == 1u);
    REQUIRE(same_result(scalar.front(), simd.front()));
    REQUIRE(scalar.front().status == static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative));
    REQUIRE(is_prefilter_accepted(scalar.front()));
}

} // namespace
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
