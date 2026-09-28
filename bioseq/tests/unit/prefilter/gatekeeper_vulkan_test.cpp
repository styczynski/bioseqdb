/**
 * @file gatekeeper_vulkan_test.cpp
 * @brief Compare Vulkan GateKeeper with scalar results and check input boundaries.
 *
 * Repeated submissions keep the same job shapes while changing input bytes
 * to check that replayed commands see the, new data.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.gpu.vulkan.runtime;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.prefilter.types;

using namespace bioseq::prefilter;

namespace {

/**
 * @brief Compute an independent exact edit distance for the GPU tests.
 *
 * Insertions, deletions, and substitutions cost one. Two matrix rows are
 * enough  to keep this test reference small
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
 * @brief Check the relationships between GateKeeper's diagnostic flags.
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

TEST_CASE(
    "GateKeeper hardware Vulkan matches the scalar oracle and replays fresh ranks",
    "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]")
{
    namespace gv = bioseq::gpu::vulkan;
    if (!is_vulkan_gatekeeper_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<prefilter_job> jobs;
    const auto append_supported_job = [
        &query_ranks,
        &target_ranks,
        &jobs](
            std::uint32_t length,
            std::uint32_t max_edits,
            std::uint32_t variant) {
        // Surround slices with extra bytes at varying offsets. This checks that
        // shaders read only the requested range when it starts within a word.
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
            // Change at  most max_edits ranks so the pair must pass.
            for (std::uint32_t edit = 0u; edit < max_edits; ++edit) {
                const std::uint32_t position =
                    (edit * 11u + 3u) % length;
                target[position] = static_cast<std::uint8_t>(
                    query[position] ^ 0xffu);
            }
        } else if (scenario == 2u) {
            // Change every ank to trigger a mask rejection and its exact check.
            for (std::uint32_t position = 0u; position < length; ++position) {
                target[position] = static_cast<std::uint8_t>(
                    query[position] ^ 0xffu);
            }
        } else if (scenario == 3u) {
            // Shift by one rank. One insertion and one deletion fit both edit limits.
            target[0] = static_cast<std::uint8_t>(query[0] ^ 0x5au);
            std::copy_n(query.begin(), length - 1u, target.begin() + 1u);
        }

        query_ranks.insert(
            query_ranks.end(), query.begin(), query.end());
        target_ranks.insert(
            target_ranks.end(), target.begin(), target.end());
        query_ranks.push_back(static_cast<std::uint8_t>(0xE0u + variant));
        target_ranks.push_back(static_cast<std::uint8_t>(0x60u + variant));
        jobs.push_back(prefilter_job{
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

    const prefilter_batch_input input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = gatekeeper_scalar_batch(input);
    auto& runtime = gv::runtime::instance();
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
            scalar[index].diagnostics,
            gpu[index].lower_bound,
            gpu[index].diagnostics);
        CHECK(same_result(gpu[index], scalar[index]));
        check_supported_diagnostics(gpu[index], jobs[index].max_edits);
    }

    // Reuse the same command shape with targets copied from the queries.
    // Replay must read the new bytes and report exact matches.
    std::vector<std::uint8_t> replay_targets = target_ranks;
    for (const prefilter_job& job : jobs) {
        std::copy_n(
            query_ranks.begin() + job.query_offset,
            job.query_length,
            replay_targets.begin() + job.target_offset);
    }
    const prefilter_batch_input replay_input{
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
        CHECK(is_prefilter_accepted(replayed[index]));
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
    "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]")
{
    namespace gv = bioseq::gpu::vulkan;
    if (!is_vulkan_gatekeeper_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    // A four-byte slice starting at offset 10 exceeds the 13 logical bytes,
    // even though four packed words have room for it. Check bounds before
    // reading bytes outside the slice or allocation padding.
    std::vector<std::uint8_t> query_ranks(13u, 0xA5u);
    std::vector<std::uint8_t> target_ranks(13u, 0x5Au);
    for (std::size_t index = 0u; index < 9u; ++index) {
        query_ranks[index] = static_cast<std::uint8_t>(index * 17u + 3u);
        target_ranks[index] = query_ranks[index];
    }
    const std::array<prefilter_job, 6u> jobs{
        prefilter_job{
            .query_offset = 10u,
            .target_offset = 9u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        prefilter_job{
            .query_offset = 9u,
            .target_offset = 10u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 4u,
        },
        prefilter_job{
            .query_offset = (std::numeric_limits<std::uint32_t>::max)(),
            .target_offset = 0u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        prefilter_job{
            .query_offset = 13u,
            .target_offset = 0u,
            .query_length = 1u,
            .target_length = 1u,
            .max_edits = 4u,
        },
        prefilter_job{
            .query_offset = 10u,
            .target_offset = 0u,
            .query_length = 4u,
            .target_length = 3u,
            .max_edits = 9u,
        },
        prefilter_job{
            .query_offset = 1u,
            .target_offset = 1u,
            .query_length = 8u,
            .target_length = 8u,
            .max_edits = 2u,
        },
    };
    const prefilter_batch_input input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    for (std::size_t index = 0u; index < jobs.size() - 1u; ++index) {
        REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs[index]));
    }
    REQUIRE(prefilter_job_in_bounds(input, jobs.back()));
    const auto scalar = gatekeeper_scalar_batch(input);
    auto& runtime = gv::runtime::instance();
    const std::uint64_t dispatches_before =
        runtime.gatekeeper_dispatch_count();
    const auto gpu = run_gatekeeper_vulkan_batch(
        query_ranks, target_ranks, jobs);

    REQUIRE(gpu.size() == jobs.size());
    REQUIRE(scalar.size() == jobs.size());
    for (std::size_t index = 0u; index < jobs.size(); ++index) {
        CAPTURE(index, gpu[index].status, gpu[index].diagnostics);
        CHECK(same_result(gpu[index], scalar[index]));
    }
    for (std::size_t index = 0u; index < jobs.size() - 1u; ++index) {
        CHECK(gpu[index].status == static_cast<std::uint32_t>(
            prefilter_status::unsupported_conservative));
        CHECK(is_prefilter_accepted(gpu[index]));
        CHECK(gpu[index].lower_bound == 0u);
        CHECK(gpu[index].diagnostics == 0u);
    }
    CHECK(runtime.gatekeeper_dispatch_count() == dispatches_before + 1u);
}

TEST_CASE(
    "GateKeeper Vulkan E2 E4 wavefront guard handles boundaries and replay",
    "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]")
{
    namespace gv = bioseq::gpu::vulkan;
    if (!is_vulkan_gatekeeper_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    auto& runtime = gv::runtime::instance();
    const std::uint64_t dispatches_before =
        runtime.gatekeeper_dispatch_count();
    std::uint64_t expected_dispatches = 0u;

    for (const std::uint32_t length : {32u, 64u}) {
        for (const std::uint32_t max_edits : {2u, 4u}) {
            std::vector<std::uint8_t> query_ranks;
            std::vector<std::uint8_t> target_ranks;
            std::vector<prefilter_job> jobs;
            std::vector<std::uint32_t> exact_distances;
            std::vector<std::uint32_t> hamming_distances;
            std::vector<bool> expected_guard_rejections;
            const auto append_case = [
                &query_ranks,
                &target_ranks,
                &jobs,
                &exact_distances,
                &hamming_distances,
                &expected_guard_rejections,
                max_edits](
                    const std::vector<std::uint8_t>& query,
                    const std::vector<std::uint8_t>& target,
                    bool expect_guard_rejection) {
                REQUIRE(query.size() == target.size());
                while ((query_ranks.size() & 3u) != 0u) {
                    query_ranks.push_back(static_cast<std::uint8_t>(
                        0xD0u + jobs.size()));
                }
                while ((target_ranks.size() & 3u) != 0u) {
                    target_ranks.push_back(static_cast<std::uint8_t>(
                        0x70u + jobs.size()));
                }
                const std::uint32_t query_offset =
                    static_cast<std::uint32_t>(query_ranks.size());
                const std::uint32_t target_offset =
                    static_cast<std::uint32_t>(target_ranks.size());
                REQUIRE((query_offset & 3u) == 0u);
                REQUIRE((target_offset & 3u) == 0u);

                query_ranks.insert(
                    query_ranks.end(), query.begin(), query.end());
                target_ranks.insert(
                    target_ranks.end(), target.begin(), target.end());
                // Add an  extra byte after the slice to catch reads past its end.
                // The next job pads its start to a packed-word boundary
                query_ranks.push_back(static_cast<std::uint8_t>(
                    0xE0u + jobs.size()));
                target_ranks.push_back(static_cast<std::uint8_t>(
                    0x60u + jobs.size()));
                jobs.push_back(prefilter_job{
                    .query_offset = query_offset,
                    .target_offset = target_offset,
                    .query_length = static_cast<std::uint32_t>(query.size()),
                    .target_length = static_cast<std::uint32_t>(target.size()),
                    .max_edits = max_edits,
                });
                exact_distances.push_back(exact_levenshtein(query, target));
                hamming_distances.push_back(static_cast<std::uint32_t>(
                    std::inner_product(
                        query.begin(),
                        query.end(),
                        target.begin(),
                        std::uint32_t{0u},
                        std::plus<>{},
                        std::not_equal_to<>{})));
                expected_guard_rejections.push_back(expect_guard_rejection);
            };

            const std::uint32_t boundary = length / 2u;
            // Rotating these three-rank runs costs two edits and creates many
            // mismatches. It forces the test past the Hamming-distance shortcut.
            std::vector<std::uint8_t> repeated_query(length);
            for (std::uint32_t position = 0u; position < length; ++position) {
                repeated_query[position] = static_cast<std::uint8_t>(
                    (position / 3u) % 11u);
            }
            std::vector<std::uint8_t> terminal_rotation(length);
            terminal_rotation[0] = repeated_query.back();
            std::copy_n(
                repeated_query.begin(),
                length - 1u,
                terminal_rotation.begin() + 1u);
            append_case(
                repeated_query,
                terminal_rotation,
                false);

            // Move one unique rank across the middle boundary. At length 64 this
            // crosses a 32-bit mask word, and, at length 32 it crosses a four-bit group.
            std::vector<std::uint8_t> boundary_query(length);
            for (std::uint32_t position = 0u; position < length; ++position) {
                boundary_query[position] = static_cast<std::uint8_t>(
                    position * 73u + 19u);
            }
            std::vector<std::uint8_t> boundary_move = boundary_query;
            const std::uint32_t move_from = boundary - 5u;
            const std::uint32_t move_to = boundary + 5u;
            const std::uint8_t moved = boundary_move[move_from];
            boundary_move.erase(
                boundary_move.begin()
                + static_cast<std::ptrdiff_t>(move_from));
            boundary_move.insert(
                boundary_move.begin()
                + static_cast<std::ptrdiff_t>(move_to),
                moved);
            append_case(
                boundary_query,
                boundary_move,
                false);

            // Leave two matching ranks across the middle boundary, surrounded by
            // mismatches. At length 64, filling this short match run crosses a  mask
            // word boundary before the exact check rejects the pair.
            std::vector<std::uint8_t> boundary_rejection(length);
            for (std::uint32_t position = 0u; position < length; ++position) {
                boundary_rejection[position] = static_cast<std::uint8_t>(
                    boundary_query[position] ^ 0xffu);
            }
            boundary_rejection[boundary - 1u] =
                boundary_query[boundary - 1u];
            boundary_rejection[boundary] = boundary_query[boundary];
            append_case(
                boundary_query,
                boundary_rejection,
                true);

            // Keep two matching ranks at the taio. Amendment must leave this end
            // run intact, while the other mismatches still cause exact rejection.
            std::vector<std::uint8_t> terminal_rejection(length);
            for (std::uint32_t position = 0u; position < length; ++position) {
                terminal_rejection[position] = static_cast<std::uint8_t>(
                    repeated_query[position] + 127u);
            }
            terminal_rejection[length - 2u] = repeated_query[length - 2u];
            terminal_rejection[length - 1u] = repeated_query[length - 1u];
            append_case(
                repeated_query,
                terminal_rejection,
                true);

            REQUIRE_FALSE(jobs.empty());
            for (const prefilter_job& job : jobs) {
                REQUIRE(job.query_length == length);
                REQUIRE(job.target_length == length);
                REQUIRE(job.max_edits == max_edits);
                REQUIRE((job.query_offset & 3u) == 0u);
                REQUIRE((job.target_offset & 3u) == 0u);
            }
            const prefilter_batch_input input{
                .query_ranks = query_ranks,
                .target_ranks = target_ranks,
                .jobs = jobs,
            };
            const auto scalar = gatekeeper_scalar_batch(input);
            REQUIRE(scalar.size() == jobs.size());
            for (std::size_t index = 0u; index < scalar.size(); ++index) {
                CAPTURE(
                    index,
                    length,
                    max_edits,
                    exact_distances[index],
                    hamming_distances[index],
                    scalar[index].lower_bound,
                    scalar[index].diagnostics);
                REQUIRE(hamming_distances[index] > max_edits);
                if (expected_guard_rejections[index]) {
                    REQUIRE(exact_distances[index] > max_edits);
                    REQUIRE_FALSE(is_prefilter_accepted(scalar[index]));
                    REQUIRE(is_gatekeeper_mask_rejection_candidate(scalar[index]));
                    REQUIRE(is_gatekeeper_exact_guard_activated(scalar[index]));
                    REQUIRE(is_gatekeeper_guard_confirmed_rejection(scalar[index]));
                } else {
                    REQUIRE(exact_distances[index] <= max_edits);
                    REQUIRE(is_prefilter_accepted(scalar[index]));
                }
            }

            const auto gpu = run_gatekeeper_vulkan_batch(
                query_ranks, target_ranks, jobs);
            ++expected_dispatches;
            REQUIRE(gpu.size() == scalar.size());
            for (std::size_t index = 0u; index < scalar.size(); ++index) {
                CAPTURE(
                    index,
                    length,
                    max_edits,
                    scalar[index].diagnostics,
                    gpu[index].diagnostics);
                CHECK(same_result(gpu[index], scalar[index]));
                check_supported_diagnostics(gpu[index], max_edits);
            }

            // Replay each length and edit-limit combination with fresh exact-match
            // targets. The changed results check that mapped input writes are visible.
            std::vector<std::uint8_t> replay_targets = target_ranks;
            for (const prefilter_job& job : jobs) {
                std::copy_n(
                    query_ranks.begin() + job.query_offset,
                    job.query_length,
                    replay_targets.begin() + job.target_offset);
            }
            const prefilter_batch_input replay_input{
                .query_ranks = query_ranks,
                .target_ranks = replay_targets,
                .jobs = jobs,
            };
            const auto replay_scalar = gatekeeper_scalar_batch(replay_input);
            const auto replayed = run_gatekeeper_vulkan_batch(
                query_ranks, replay_targets, jobs);
            ++expected_dispatches;

            REQUIRE(replayed.size() == replay_scalar.size());
            bool observed_changed_result = false;
            for (std::size_t index = 0u;
                 index < replay_scalar.size();
                 ++index) {
                CAPTURE(index, length, max_edits);
                CHECK(same_result(replayed[index], replay_scalar[index]));
                CHECK(is_prefilter_accepted(replayed[index]));
                CHECK(replayed[index].lower_bound == 0u);
                CHECK(gatekeeper_diagnostic_estimate(replayed[index]) == 0u);
                observed_changed_result |=
                    !same_result(scalar[index], replay_scalar[index]);
            }
            CHECK(observed_changed_result);
        }
    }
    CHECK(expected_dispatches == 8u);
    CHECK(runtime.gatekeeper_dispatch_count()
          == dispatches_before + expected_dispatches);
}

} // namespace
