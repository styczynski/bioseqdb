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
import bioseq.prefilter.sneaky_snake; // TEMPORARY_NATIVE_REGISTRY_PREFILTER_PROBE
import bioseq.prefilter.sneaky_snake.vulkan; // TEMPORARY_NATIVE_REGISTRY_PREFILTER_PROBE
import bioseq.prefilter.types;
import bioseq.sequences;
import bioseq.sequences.kmers.vulkan_minimizers; // TEMPORARY_NATIVE_REGISTRY_PREFILTER_PROBE
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.common.seqan_adapter;
import bioseq.highway.test_support;

using namespace bioseq::prefilter;

namespace {

/** Borrows native aa27 fixture ranks for the checked CPU batch interface. */
struct rank_batch {
    std::span<const std::uint8_t> query_ranks;
    std::span<const std::uint8_t> target_ranks;
    std::span<const prefilter_job> jobs;
    static const std::uint8_t* read(const void* state, bioseq::storage::sequence_size_t from,
                                  std::size_t, std::span<std::uint8_t>) {
        return static_cast<const std::span<const std::uint8_t>*>(state)->data() + from;
    }
    std::array<bioseq::storage::borrowed_rank_window_source, 1> queries{{{
        .context = &query_ranks, .read_at = &read, .symbol_count = query_ranks.size(),
        .alphabet_code = supported_biological_alphabets::index_of<seq::aa27>(),
        .worker_shareable = true}}};
    std::array<bioseq::storage::borrowed_rank_window_source, 1> targets{{{
        .context = &target_ranks, .read_at = &read, .symbol_count = target_ranks.size(),
        .alphabet_code = supported_biological_alphabets::index_of<seq::aa27>(),
        .worker_shareable = true}}};
    operator prefilter_batch_input() const { return {queries, targets, jobs}; }
};

/** Requires checked CPU input before comparing numerical results. */
template <class Value, class Error>
[[nodiscard]] Value checked_value(std::expected<Value, Error> result) {
    REQUIRE(result.has_value());
    return std::move(*result);
}

/** Presents existing byte fixtures as native aa27 values to the packed GPU adapter. */
[[nodiscard]] std::vector<prefilter_result> run_fixture_gpu(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const prefilter_job> jobs) {
    std::vector<seq::aa27> query_letters(query_ranks.size());
    std::vector<seq::aa27> target_letters(target_ranks.size());
    for (std::size_t index = 0; index < query_ranks.size(); ++index)
        seqan3::assign_rank_to(query_ranks[index] % seqan3::alphabet_size<seq::aa27>, query_letters[index]);
    for (std::size_t index = 0; index < target_ranks.size(); ++index)
        seqan3::assign_rank_to(target_ranks[index] % seqan3::alphabet_size<seq::aa27>, target_letters[index]);
    const compact_sequence_range<seq::aa27> query{query_letters};
    const compact_sequence_range<seq::aa27> target{target_letters};
    return run_gatekeeper_vulkan_batch(bioseq::storage::make_lazy_sequence_view(query),
                                       bioseq::storage::make_lazy_sequence_view(target), jobs, ::bioseq::simd_dispatch::highway_test_kernel_table().common.validate_packed_sequence_ranks);
}

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
            static_cast<std::uint8_t>((0xD0u + variant) % seqan3::alphabet_size<seq::aa27>));
        target_ranks.insert(
            target_ranks.end(), 2u + variant % 2u,
            static_cast<std::uint8_t>((0x70u + variant) % seqan3::alphabet_size<seq::aa27>));
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            query_ranks.size());
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            target_ranks.size());

        std::vector<std::uint8_t> query(length);
        std::vector<std::uint8_t> target(length);
        for (std::uint32_t position = 0u; position < length; ++position) {
            query[position] = static_cast<std::uint8_t>((
                (position * 73u + variant * 41u + length) & 0xffu) % seqan3::alphabet_size<seq::aa27>);
        }
        target = query;
        const std::uint32_t scenario = variant % 4u;
        if (scenario == 1u) {
            // Change at  most max_edits ranks so the pair must pass.
            for (std::uint32_t edit = 0u; edit < max_edits; ++edit) {
                const std::uint32_t position =
                    (edit * 11u + 3u) % length;
                target[position] = static_cast<std::uint8_t>((
                    query[position] ^ 0xffu) % seqan3::alphabet_size<seq::aa27>);
            }
        } else if (scenario == 2u) {
            // Change every ank to trigger a mask rejection and its exact check.
            for (std::uint32_t position = 0u; position < length; ++position) {
                target[position] = static_cast<std::uint8_t>((
                    query[position] ^ 0xffu) % seqan3::alphabet_size<seq::aa27>);
            }
        } else if (scenario == 3u) {
            // Shift by one rank. One insertion and one deletion fit both edit limits.
            target[0] = static_cast<std::uint8_t>((query[0] ^ 0x5au) % seqan3::alphabet_size<seq::aa27>);
            std::copy_n(query.begin(), length - 1u, target.begin() + 1u);
        }

        query_ranks.insert(
            query_ranks.end(), query.begin(), query.end());
        target_ranks.insert(
            target_ranks.end(), target.begin(), target.end());
        query_ranks.push_back(static_cast<std::uint8_t>((0xE0u + variant) % seqan3::alphabet_size<seq::aa27>));
        target_ranks.push_back(static_cast<std::uint8_t>((0x60u + variant) % seqan3::alphabet_size<seq::aa27>));
        jobs.push_back(prefilter_job{
            .query_start = query_offset,
            .target_start = target_offset,
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

    const rank_batch input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = checked_value(gatekeeper_scalar_batch(input));
    auto& runtime = gv::runtime::instance();
    const std::uint64_t dispatches_before =
        runtime.gatekeeper_dispatch_count();
    const auto gpu = run_fixture_gpu(
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
            query_ranks.begin() + job.query_start,
            job.query_length,
            replay_targets.begin() + job.target_start);
    }
    const rank_batch replay_input{
        .query_ranks = query_ranks,
        .target_ranks = replay_targets,
        .jobs = jobs,
    };
    const auto replay_scalar = checked_value(gatekeeper_scalar_batch(replay_input));
    const auto replayed = run_fixture_gpu(
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
    "GateKeeper hardware Vulkan rejects invalid coordinates before dispatch",
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
        query_ranks[index] = static_cast<std::uint8_t>((index * 17u + 3u) % seqan3::alphabet_size<seq::aa27>);
        target_ranks[index] = query_ranks[index];
    }
    const std::array<prefilter_job, 6u> jobs{
        prefilter_job{
            .query_start = 10u,
            .target_start = 9u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        prefilter_job{
            .query_start = 9u,
            .target_start = 10u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 4u,
        },
        prefilter_job{
            .query_start = (std::numeric_limits<std::uint32_t>::max)(),
            .target_start = 0u,
            .query_length = 4u,
            .target_length = 4u,
            .max_edits = 2u,
        },
        prefilter_job{
            .query_start = 13u,
            .target_start = 0u,
            .query_length = 1u,
            .target_length = 1u,
            .max_edits = 4u,
        },
        prefilter_job{
            .query_start = 10u,
            .target_start = 0u,
            .query_length = 4u,
            .target_length = 3u,
            .max_edits = 9u,
        },
        prefilter_job{
            .query_start = 1u,
            .target_start = 1u,
            .query_length = 8u,
            .target_length = 8u,
            .max_edits = 2u,
        },
    };
    const rank_batch input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    for (std::size_t index = 0u; index < jobs.size() - 1u; ++index) {
        REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs[index]));
    }
    REQUIRE(prefilter_job_in_bounds(input, jobs.back()));
    const auto scalar = gatekeeper_scalar_batch(input);
    REQUIRE_FALSE(scalar.has_value());
    auto& runtime = gv::runtime::instance();
    const std::uint64_t dispatches_before = runtime.gatekeeper_dispatch_count();
    CHECK_THROWS(run_fixture_gpu(query_ranks, target_ranks, jobs));
    CHECK(runtime.gatekeeper_dispatch_count() == dispatches_before);

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
                    query_ranks.push_back(static_cast<std::uint8_t>((
                        0xD0u + jobs.size()) % seqan3::alphabet_size<seq::aa27>));
                }
                while ((target_ranks.size() & 3u) != 0u) {
                    target_ranks.push_back(static_cast<std::uint8_t>((
                        0x70u + jobs.size()) % seqan3::alphabet_size<seq::aa27>));
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
                query_ranks.push_back(static_cast<std::uint8_t>((
                    0xE0u + jobs.size()) % seqan3::alphabet_size<seq::aa27>));
                target_ranks.push_back(static_cast<std::uint8_t>((
                    0x60u + jobs.size()) % seqan3::alphabet_size<seq::aa27>));
                jobs.push_back(prefilter_job{
                    .query_start = query_offset,
                    .target_start = target_offset,
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
                repeated_query[position] = static_cast<std::uint8_t>((
                    (position / 3u) % 11u) % seqan3::alphabet_size<seq::aa27>);
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
                boundary_query[position] = static_cast<std::uint8_t>((
                    position * 73u + 19u) % seqan3::alphabet_size<seq::aa27>);
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
                boundary_rejection[position] = static_cast<std::uint8_t>((
                    boundary_query[position] ^ 0xffu) % seqan3::alphabet_size<seq::aa27>);
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
                terminal_rejection[position] = static_cast<std::uint8_t>((
                    repeated_query[position] + 127u) % seqan3::alphabet_size<seq::aa27>);
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
                REQUIRE((job.query_start & 3u) == 0u);
                REQUIRE((job.target_start & 3u) == 0u);
            }
            const rank_batch input{
                .query_ranks = query_ranks,
                .target_ranks = target_ranks,
                .jobs = jobs,
            };
            const auto scalar = checked_value(gatekeeper_scalar_batch(input));
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

            const auto gpu = run_fixture_gpu(
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
                    query_ranks.begin() + job.query_start,
                    job.query_length,
                    replay_targets.begin() + job.target_start);
            }
            const rank_batch replay_input{
                .query_ranks = query_ranks,
                .target_ranks = replay_targets,
                .jobs = jobs,
            };
            const auto replay_scalar = checked_value(gatekeeper_scalar_batch(replay_input));
            const auto replayed = run_fixture_gpu(
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

// BEGIN_TEMPORARY_NATIVE_REGISTRY_PREFILTER_PROBE -- DO NOT COMMIT
// Ordinary owner only: no generated Highway test identity is introduced.
TEST_CASE("Manual native registry four-width prefilter adapters",
          "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]") {
    namespace gv = bioseq::gpu::vulkan;
    namespace bs = bioseq::storage;
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan initialization: " << runtime.initialization_error());
    REQUIRE(is_vulkan_gatekeeper_supported());
    REQUIRE(is_vulkan_sneaky_snake_supported());
    REQUIRE_FALSE(runtime.features().software_device);
    std::size_t checked_profiles = 0u, checked_jobs = 0u, checked_reverse_jobs = 0u, checked_leading_offsets = 0u;
    std::size_t checked_width_selector_profiles = 0u, checked_unused_width_rejections = 0u;
    const auto exercise = [&runtime, &checked_profiles, &checked_jobs,
                           &checked_reverse_jobs, &checked_leading_offsets,
                           &checked_width_selector_profiles, &checked_unused_width_rejections]<sequence_alphabet Alphabet, class Word>() {
        using range_type = compact_sequence_range<sequence_custom_alphabet<Alphabet, Word>>;
        using traits = typename range_type::traits;
        constexpr auto cardinality = seqan3::alphabet_size<Alphabet>;
        constexpr auto letters_per_unit = traits::letters_per_unit;
        constexpr auto alphabet_code = supported_biological_alphabets::template index_of<Alphabet>();
        CAPTURE(alphabet_code, sizeof(Word), letters_per_unit);
        std::vector<std::uint8_t> query_ranks, target_ranks;
        std::vector<prefilter_job> jobs;
        const auto append = [&query_ranks, &target_ranks, &jobs](
            std::span<const std::uint8_t> query, std::span<const std::uint8_t> target,
            std::uint32_t max_edits, std::size_t leading = std::numeric_limits<std::size_t>::max()) {
            const auto query_padding = leading == std::numeric_limits<std::size_t>::max()
                ? 1u + jobs.size() % (letters_per_unit + 1u)
                : (leading + letters_per_unit - query_ranks.size() % letters_per_unit) % letters_per_unit;
            const auto target_padding = leading == std::numeric_limits<std::size_t>::max()
                ? 2u + jobs.size() % (letters_per_unit + 1u)
                : ((leading + 1u) % letters_per_unit + letters_per_unit - target_ranks.size() % letters_per_unit) % letters_per_unit;
            query_ranks.insert(query_ranks.end(), query_padding, static_cast<std::uint8_t>(cardinality - 1u));
            target_ranks.insert(target_ranks.end(), target_padding, 0u);
            const auto query_start = static_cast<std::uint32_t>(query_ranks.size());
            const auto target_start = static_cast<std::uint32_t>(target_ranks.size());
            query_ranks.insert(query_ranks.end(), query.begin(), query.end());
            target_ranks.insert(target_ranks.end(), target.begin(), target.end());
            jobs.push_back(prefilter_job{.query_start = query_start, .target_start = target_start,
                .query_length = static_cast<std::uint32_t>(query.size()),
                .target_length = static_cast<std::uint32_t>(target.size()), .max_edits = max_edits});
        };
        constexpr std::array<std::size_t, 12> lengths{
            0u, 1u, letters_per_unit - 1u, letters_per_unit, letters_per_unit + 1u,
            31u, 32u, 33u, 63u, 64u, 65u, 129u};
        for (const auto length : lengths) {
            std::vector<std::uint8_t> query(length);
            for (std::size_t position = 0u; position < length; ++position)
                query[position] = static_cast<std::uint8_t>(cardinality - 1u - position % cardinality);
            auto changed = query;
            if (!changed.empty()) changed[length / 2u] = static_cast<std::uint8_t>(
                (changed[length / 2u] + 1u) % cardinality);
            for (const std::uint32_t max_edits : {0u, 1u}) {
                append(query, query, max_edits);
                append(query, changed, max_edits);
            }
        }
        const std::vector<std::uint8_t> high(64u, static_cast<std::uint8_t>(cardinality - 1u));
        const std::vector<std::uint8_t> low(64u, 0u);
        for (const std::uint32_t max_edits : {2u, 4u}) {
            append(high, high, max_edits);
            append(high, low, max_edits); // Independent distance 64: both kernels must reject.
        }
        append(high, high, 8u);
        append(high, high, 9u); // GateKeeper's supported/unsupported threshold boundary.
        auto high_bit_changed = high;
        high_bit_changed[32u] ^= static_cast<std::uint8_t>(1u << (traits::bits_per_char - 1u));
        REQUIRE(high_bit_changed[32u] < cardinality);
        append(high, high_bit_changed, 0u);
        append(high, high_bit_changed, 1u);
        append(high, high, 31u);
        append(high, high, 32u); // SneakySnake GPU domain boundary: explicit conservative keep.
        const std::vector<std::uint8_t> unequal(65u, static_cast<std::uint8_t>(cardinality - 1u));
        append(high, unequal, 0u);
        const auto high32 = std::span<const std::uint8_t>{high}.first(32u);
        for (std::size_t leading = 0u; leading < letters_per_unit; ++leading) {
            append(high32, high32, 0u, leading);
            CHECK(jobs.back().query_start % letters_per_unit == leading);
            CHECK(jobs.back().target_start % letters_per_unit == (leading + 1u) % letters_per_unit);
            ++checked_leading_offsets;
        }
        // Retain a partial final word when the layout contains multiple letters per unit.
        query_ranks.push_back(0u);
        if constexpr (letters_per_unit > 1u)
            if (query_ranks.size() % letters_per_unit == 0u) query_ranks.push_back(0u);
        if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
            const auto found = std::ranges::find_if(jobs, [](const prefilter_job& job) {
                return job.query_length == 32u && job.max_edits == 0u;
            });
            REQUIRE(found != jobs.end());
            const auto forward_start = found->query_start;
            const auto target_start = static_cast<std::uint32_t>(target_ranks.size());
            for (std::size_t position = 0u; position < 32u; ++position) {
                Alphabet letter{};
                seqan3::assign_rank_to(query_ranks[forward_start + 31u - position], letter);
                target_ranks.push_back(static_cast<std::uint8_t>(seqan3::to_rank(seqan3::complement(letter))));
            }
            jobs.push_back(prefilter_job{.query_record_index = 1u,
                .query_start = static_cast<std::uint32_t>(query_ranks.size() - forward_start - 32u),
                .target_start = target_start, .query_length = 32u, .target_length = 32u,
                .max_edits = 0u, .orientation = static_cast<std::uint32_t>(bs::sequence_transform::reverse_complement)});
        }
        target_ranks.push_back(0u);
        if constexpr (letters_per_unit > 1u)
            if (target_ranks.size() % letters_per_unit == 0u) target_ranks.push_back(0u);
        const auto pack = [](std::span<const std::uint8_t> ranks) {
            std::vector<Alphabet> letters(ranks.size());
            for (std::size_t index = 0u; index < ranks.size(); ++index)
                seqan3::assign_rank_to(ranks[index], letters[index]);
            return range_type{letters};
        };
        const auto query = pack(query_ranks), target = pack(target_ranks);
        const auto query_view = bs::make_lazy_sequence_view(query);
        const auto target_view = bs::make_lazy_sequence_view(target);
        std::array query_sources{query_view.template borrow_rank_source<false>(),
                                 query_view.template borrow_rank_source<false>()};
        if constexpr (seqan3::nucleotide_alphabet<Alphabet>)
            query_sources[1] = query_view.template borrow_rank_source<true>();
        const std::array target_sources{target_view.template borrow_rank_source<false>()};
        const prefilter_batch_input input{query_sources, target_sources, jobs};
        const auto scalar_gate = checked_value(gatekeeper_scalar_batch(input));
        const auto scalar_snake = checked_value(sneaky_snake_scalar_batch(input));
        // Verify native borrowing is available in the requested source width and
        // at the exact offsets the public packed-window GPU adapter receives.
        std::size_t unaligned_windows = 0u;
        for (const auto& job : jobs) {
            if (job.query_length == 0u) continue;
            const auto begin = job.orientation == 0u ? job.query_start
                : query_ranks.size() - job.query_start - job.query_length;
            const auto descriptor = query_view.try_resident_packed(begin, begin + job.query_length, sizeof(Word));
            REQUIRE(descriptor.has_value());
            CHECK(descriptor->storage_unit_width == sizeof(Word));
            CHECK(descriptor->bits_per_symbol == traits::bits_per_char);
            CHECK(descriptor->leading_symbol_offset == begin % letters_per_unit);
            unaligned_windows += descriptor->leading_symbol_offset != 0u;
        }
        if constexpr (letters_per_unit > 1u) {
            CHECK(unaligned_windows > 0u);
            CHECK(query.padding() > 0u);
            CHECK(target.padding() > 0u);
        }
        const auto validate_ranks = ::bioseq::simd_dispatch::highway_test_kernel_table()
            .common.validate_packed_sequence_ranks;
        const auto gate_before = runtime.gatekeeper_dispatch_count();
        const auto gpu_gate = run_gatekeeper_vulkan_batch(query_view, target_view, jobs, validate_ranks);
        REQUIRE(runtime.gatekeeper_dispatch_count() > gate_before);
        const auto snake_before = runtime.sneaky_snake_dispatch_count();
        const auto gpu_snake = run_sneaky_snake_vulkan_batch(query_view, target_view, jobs, validate_ranks);
        REQUIRE(runtime.sneaky_snake_dispatch_count() > snake_before);
        REQUIRE(gpu_gate.size() == jobs.size());
        REQUIRE(gpu_snake.size() == jobs.size());
        // TEMPORARY: validate selector admission and numerical parity on the existing
        // u32 profile for each alphabet; keep the original corpus and case count.
        if constexpr (std::is_same_v<Word, std::uint32_t>) {
            constexpr auto rank_bits = static_cast<std::uint32_t>(traits::bits_per_char);
            const auto selector_dispatches_before = runtime.sneaky_snake_dispatch_count();
            std::size_t consumers = 0u;
            const auto consumed = gv::with_prefilter_packed_windows(query_view, target_view, jobs,
                [&runtime, validate_ranks, rank_bits, &gpu_snake, &consumers,
                 &checked_unused_width_rejections](auto queries, auto targets,
                                                   const auto& rank_lut, auto local_jobs) {
                    ++consumers;
                    REQUIRE_FALSE(queries.empty());
                    REQUIRE(local_jobs.size() == gpu_snake.size());
                    const auto check_results = [&gpu_snake](const auto& actual) {
                        REQUIRE(actual.size() == gpu_snake.size());
                        for (std::size_t index = 0u; index < actual.size(); ++index) {
                            CAPTURE(index);
                            CHECK(same_result(actual[index], gpu_snake[index]));
                        }
                    };
                    // A legal, unused wider record must be accepted by dynamic0
                    // and rejected by B. Its one active rank is independently zero;
                    // no alphabet-sized fixture, fabricated address or bad geometry.
                    std::vector<gv::gpu_packed_sequence_upload> raw_queries(queries.begin(), queries.end());
                    const std::array<std::uint32_t, 1u> unused_native_unit{};
                    if constexpr (rank_bits < std::numeric_limits<unsigned char>::digits) {
                        auto unused = queries.front();
                        auto& descriptor = unused.sequence;
                        descriptor.seq_word_offset = 0u;
                        descriptor.byte_count = sizeof(std::uint32_t);
                        descriptor.padded_byte_count = sizeof(std::uint32_t);
                        descriptor.unit_count = unused_native_unit.size();
                        descriptor.bp_offset = 0u;
                        descriptor.bp_count = 1u;
                        descriptor.bits_per_char = rank_bits + 1u;
                        descriptor.unit_bits = std::numeric_limits<std::uint32_t>::digits;
                        descriptor.letters_per_unit = descriptor.unit_bits / descriptor.bits_per_char;
                        descriptor.char_mask = (std::uint32_t{1u} << descriptor.bits_per_char) - 1u;
                        descriptor.leading_symbol_offset = 0u;
                        descriptor.padding_letters = descriptor.letters_per_unit - 1u;
                        unused.bytes = std::as_bytes(std::span{unused_native_unit});
                        REQUIRE(gv::packed_sequence_upload_geometry_is_valid(unused));
                        REQUIRE(gv::packed_sequence_upload_is_valid(unused));
                        REQUIRE(validate_ranks(unused.bytes, sizeof(std::uint32_t), descriptor.bits_per_char,
                            descriptor.alphabet_size, descriptor.leading_symbol_offset, descriptor.bp_count));
                        for (const auto& job : local_jobs)
                            REQUIRE(job.query_record_index < raw_queries.size());
                        raw_queries.push_back(unused);
                    }
                    const auto before_dynamic = runtime.sneaky_snake_dispatch_count();
                    check_results(runtime.run_sneaky_snake_rank_batch_roundtrip(
                        raw_queries, targets, rank_lut, local_jobs, validate_ranks, 0u));
                    CHECK(runtime.sneaky_snake_dispatch_count() == before_dynamic + 1u);
                    if constexpr (rank_bits < std::numeric_limits<unsigned char>::digits) {
                        const auto before_rejection = runtime.sneaky_snake_dispatch_count();
                        REQUIRE_THROWS_AS(runtime.run_sneaky_snake_rank_batch_roundtrip(
                            raw_queries, targets, rank_lut, local_jobs, validate_ranks, rank_bits), gv::vulkan_runtime_error);
                        CHECK(runtime.sneaky_snake_dispatch_count() == before_rejection);
                        ++checked_unused_width_rejections;
                    }
                    // Dynamic0 -> B switches the existing pipeline key; the next B
                    // call repeats the same recording shape. Assert observable results
                    // and completed calls, not an unexposed re-recording counter.
                    for (std::size_t repeat = 0u; repeat < 2u; ++repeat) {
                        CAPTURE(repeat);
                        const auto before_repeat = runtime.sneaky_snake_dispatch_count();
                        check_results(runtime.run_sneaky_snake_rank_batch_roundtrip(
                            queries, targets, rank_lut, local_jobs, validate_ranks, rank_bits));
                        CHECK(runtime.sneaky_snake_dispatch_count() == before_repeat + 1u);
                    }
                    // The default descriptor has no payload and is invalid for a
                    // nonempty batch. Empty jobs must return before its validation.
                    static std::size_t empty_validation_calls = 0u;
                    empty_validation_calls = 0u;
                    const auto reject_if_called = +[](std::span<const std::byte>, std::size_t,
                        std::size_t, std::size_t, std::size_t, std::size_t) noexcept {
                        ++empty_validation_calls;
                        return false;
                    };
                    const std::array<gv::gpu_packed_sequence_upload, 1u> empty_descriptors{};
                    const auto before_empty = runtime.sneaky_snake_dispatch_count();
                    const auto empty = runtime.run_sneaky_snake_rank_batch_roundtrip(
                        empty_descriptors, empty_descriptors, rank_lut,
                        std::span<const prefilter_job>{}, reject_if_called, rank_bits);
                    CHECK(empty.empty());
                    CHECK(empty_validation_calls == 0u);
                    CHECK(runtime.sneaky_snake_dispatch_count() == before_empty);
                    return true;
                });
            CHECK(consumed);
            CHECK(consumers == 1u);
            CHECK(runtime.sneaky_snake_dispatch_count() == selector_dispatches_before + 3u);
            ++checked_width_selector_profiles;
        }
        std::size_t gate_rejections = 0u, snake_rejections = 0u;
        for (std::size_t index = 0u; index < jobs.size(); ++index) {
            const auto& job = jobs[index];
            CAPTURE(index, job.query_length, job.target_length, job.max_edits, job.orientation);
            std::vector<std::uint8_t> logical_query(job.query_length);
            for (std::size_t position = 0u; position < logical_query.size(); ++position) {
                if (job.orientation == 0u) logical_query[position] = query_ranks[job.query_start + position];
                else if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
                    Alphabet letter{};
                    seqan3::assign_rank_to(query_ranks[query_ranks.size() - 1u - job.query_start - position], letter);
                    logical_query[position] = static_cast<std::uint8_t>(seqan3::to_rank(seqan3::complement(letter)));
                }
            }
            const auto logical_target = std::span<const std::uint8_t>{target_ranks}.subspan(job.target_start, job.target_length);
            const auto distance = exact_levenshtein(logical_query, logical_target);
            REQUIRE(same_result(gpu_gate[index], scalar_gate[index]));
            if (job.max_edits <= gv::k_gpu_sneaky_snake_max_edits)
                REQUIRE(same_result(gpu_snake[index], scalar_snake[index]));
            else {
                CHECK(gpu_snake[index].status == static_cast<std::uint32_t>(prefilter_status::unsupported_conservative));
                CHECK(gpu_snake[index].lower_bound == 0u);
                CHECK(is_prefilter_accepted(gpu_snake[index]));
            }
            for (const auto& result : {gpu_gate[index], gpu_snake[index]}) {
                CHECK(result.lower_bound <= distance);
                if (distance <= job.max_edits) CHECK(is_prefilter_accepted(result));
                if (!is_prefilter_accepted(result)) CHECK(distance > job.max_edits);
            }
            if (job.query_length == 64u && (job.max_edits == 2u || job.max_edits == 4u)) {
                REQUIRE((distance == 0u || distance == 64u));
                CHECK(is_prefilter_accepted(gpu_gate[index]) == (distance == 0u));
                CHECK(is_prefilter_accepted(gpu_snake[index]) == (distance == 0u));
            }
            if (job.query_length == job.target_length && job.query_length <= gv::k_gpu_gatekeeper_max_length
                && job.max_edits == 0u) {
                CHECK(is_prefilter_accepted(gpu_gate[index]) == (distance == 0u));
                CHECK(is_prefilter_accepted(gpu_snake[index]) == (distance == 0u));
            }
            gate_rejections += !is_prefilter_accepted(gpu_gate[index]);
            snake_rejections += !is_prefilter_accepted(gpu_snake[index]);
            if (job.orientation != 0u) {
                CHECK(distance == 0u);
                CHECK(gpu_gate[index].lower_bound == 0u);
                CHECK(gpu_snake[index].lower_bound == 0u);
                ++checked_reverse_jobs;
            }
        }
        CHECK(gate_rejections > 0u);
        CHECK(snake_rejections > 0u);
        // Checked noPG failures are failures, never unsupported successful keeps.
        const std::array failure_jobs{prefilter_job{.query_length = 1u, .target_length = 1u}};
        auto failed_source = query_sources[0];
        failed_source.read_at = +[](const void*, bs::sequence_size_t, std::size_t,
                                    std::span<std::uint8_t>) -> const std::uint8_t* { return nullptr; };
        std::array failure_sources{failed_source};
        for (const bool invalid_rank : {false, true}) {
            const std::uint8_t invalid = cardinality;
            if (invalid_rank) {
                failure_sources[0].context = &invalid;
                failure_sources[0].read_at = +[](const void* context, bs::sequence_size_t,
                    std::size_t count, std::span<std::uint8_t> scratch) -> const std::uint8_t* {
                    std::fill_n(scratch.begin(), count, *static_cast<const std::uint8_t*>(context));
                    return scratch.data();
                };
            }
            const prefilter_batch_input invalid_input{failure_sources, target_sources, failure_jobs};
            const auto gate = gatekeeper_scalar_batch(invalid_input);
            const auto snake = sneaky_snake_scalar_batch(invalid_input);
            REQUIRE_FALSE(gate.has_value());
            REQUIRE_FALSE(snake.has_value());
            const auto expected = invalid_rank ? prefilter_input_failure_code::invalid_rank
                                               : prefilter_input_failure_code::source_read_failed;
            CHECK(gate.error().code == expected);
            CHECK(snake.error().code == expected);
        }
        if constexpr (!seqan3::nucleotide_alphabet<Alphabet>) {
            auto invalid_job = failure_jobs.front();
            invalid_job.query_record_index = 1u;
            invalid_job.orientation = static_cast<std::uint32_t>(bs::sequence_transform::reverse_complement);
            query_sources[1].orientation = bs::sequence_transform::reverse_complement;
            const std::array invalid_jobs{invalid_job};
            const auto gate = gatekeeper_scalar_batch({query_sources, target_sources, invalid_jobs});
            const auto snake = sneaky_snake_scalar_batch({query_sources, target_sources, invalid_jobs});
            REQUIRE_FALSE(gate.has_value());
            REQUIRE_FALSE(snake.has_value());
            CHECK(gate.error().code == prefilter_input_failure_code::invalid_descriptor);
            CHECK(snake.error().code == prefilter_input_failure_code::invalid_descriptor);
            const auto gate_before_invalid = runtime.gatekeeper_dispatch_count();
            const auto snake_before_invalid = runtime.sneaky_snake_dispatch_count();
            REQUIRE_THROWS_AS(run_gatekeeper_vulkan_batch(query_view, target_view, invalid_jobs, validate_ranks), gv::vulkan_runtime_error);
            REQUIRE_THROWS_AS(run_sneaky_snake_vulkan_batch(query_view, target_view, invalid_jobs, validate_ranks), gv::vulkan_runtime_error);
            CHECK(runtime.gatekeeper_dispatch_count() == gate_before_invalid);
            CHECK(runtime.sneaky_snake_dispatch_count() == snake_before_invalid);
        }
        ++checked_profiles;
        checked_jobs += jobs.size();
    };
    const auto exercise_alphabet = [&exercise]<sequence_alphabet Alphabet>() {
        using entry = supported_biological_alphabets::entry_for<Alphabet>;
        using header = compact_sequence_stdc<supported_alphabets<entry>>;
        [&exercise]<std::size_t... Widths>(supported_storage_unit_byte_widths<Widths...>) {
            (exercise.template operator()<Alphabet,
                typename compact_sequence_stdc_instance<header, Alphabet, Widths>::storage_unit>(), ...);
        }(all_storage_unit_byte_widths{});
    };
    std::apply([&exercise_alphabet](auto... entry) {
        (exercise_alphabet.template operator()<typename decltype(entry)::alphabet_type>(), ...);
    }, supported_biological_alphabets::alphabet_entries_tuple{});
    constexpr auto profiles = all_storage_unit_byte_widths::width_count
        * std::tuple_size_v<supported_biological_alphabets::alphabet_entries_tuple>;
    constexpr auto reverse_profiles = all_storage_unit_byte_widths::width_count
        * []<class... Entries>(std::tuple<Entries...>) {
            return (std::size_t{} + ... + static_cast<std::size_t>(Entries::is_complementable));
        }(supported_biological_alphabets::alphabet_entries_tuple{});
    constexpr auto entry_leading_offsets = []<class Entry>() {
        using alphabet = typename Entry::alphabet_type;
        using header = compact_sequence_stdc<supported_alphabets<Entry>>;
        return []<std::size_t... Widths>(supported_storage_unit_byte_widths<Widths...>) {
            return (std::size_t{} + ... + compact_sequence_range<sequence_custom_alphabet<alphabet,
                typename compact_sequence_stdc_instance<header, alphabet, Widths>::storage_unit>>::traits::letters_per_unit);
        }(all_storage_unit_byte_widths{});
    };
    constexpr auto leading_offsets = std::apply([entry_leading_offsets](auto... entry) {
        return (std::size_t{} + ... + entry_leading_offsets.template operator()<decltype(entry)>());
    }, supported_biological_alphabets::alphabet_entries_tuple{});
    CHECK(checked_width_selector_profiles ==
          std::tuple_size_v<supported_biological_alphabets::alphabet_entries_tuple>);
    constexpr auto widenable_alphabets = []<class... Entries>(std::tuple<Entries...>) {
        return (std::size_t{} + ... + static_cast<std::size_t>(
            Entries::rank_bits < std::numeric_limits<unsigned char>::digits));
    }(supported_biological_alphabets::alphabet_entries_tuple{});
    CHECK(checked_unused_width_rejections == widenable_alphabets);
    CHECK(checked_profiles == profiles);
    CHECK(checked_reverse_jobs == reverse_profiles);
    CHECK(checked_leading_offsets == leading_offsets);
    CHECK(checked_jobs == profiles * 59u + leading_offsets + reverse_profiles);
}
// END_TEMPORARY_NATIVE_REGISTRY_PREFILTER_PROBE

TEST_CASE("Packed SneakySnake obeys complement policies across transport boundaries",
          "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]") {
    namespace gv = bioseq::gpu::vulkan;
    namespace bs = bioseq::storage;
    if (!is_vulkan_sneaky_snake_supported()) SKIP("hardware Vulkan device unavailable");
    auto& runtime = gv::runtime::instance();
    const auto validator = ::bioseq::simd_dispatch::highway_test_kernel_table()
        .common.validate_packed_sequence_ranks;
    const auto exercise = [&]<class Word>() {
        using range_type = compact_sequence_range<sequence_custom_alphabet<seq::dna4, Word>>;
        constexpr std::size_t extent = 4096u;
        constexpr std::array<std::uint32_t, 14> lengths{
            1u, 2u, 3u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 65u};
        // The last permutation is deliberately not an XOR mapping. The shader
        // must use the supplied policy, including its general lookup fallback.
        constexpr std::array<std::array<std::uint32_t, 4>, 4> policies{{
            {{0u, 1u, 2u, 3u}}, {{1u, 0u, 3u, 2u}},
            {{3u, 2u, 1u, 0u}}, {{0u, 1u, 3u, 2u}}}};
        std::vector<seq::dna4> query_letters(extent);
        for (std::size_t i = 0; i < extent; ++i)
            seqan3::assign_rank_to((13u * i + i / 7u) % 4u, query_letters[i]);
        const range_type query{query_letters};
        for (const auto policy : policies) {
            std::vector<std::uint8_t> oriented(extent), target_ranks(extent, 0u);
            for (std::size_t i = 0; i < extent; ++i)
                oriented[i] = policy[seqan3::to_rank(query_letters[extent - 1u - i])];
            for (const auto max_edits : {0u, 16u}) {
                std::vector<prefilter_job> jobs;
                std::vector<prefilter_result> expected;
                for (std::size_t i = 0; i < lengths.size(); ++i) {
                    const auto qstart = static_cast<std::uint32_t>(3u + 97u * i);
                    const auto tstart = static_cast<std::uint32_t>(5u + 97u * i);
                    const auto length = lengths[i];
                    std::copy_n(oriented.begin() + qstart, length, target_ranks.begin() + tstart);
                    if (i % 2u != 0u)
                        target_ranks[tstart + length / 2u] =
                            (target_ranks[tstart + length / 2u] + 1u) % 4u;
                    jobs.push_back({.query_record_index = 1u, .target_record_index = 0u,
                        .query_start = qstart, .target_start = tstart,
                        .query_length = length, .target_length = length,
                        .max_edits = max_edits, .orientation = 3u});
                    expected.push_back(sneaky_snake_scalar(
                        std::span{oriented}.subspan(qstart, length),
                        std::span{target_ranks}.subspan(tstart, length), max_edits));
                }
                std::vector<seq::dna4> target_letters(extent);
                for (std::size_t i = 0; i < extent; ++i)
                    seqan3::assign_rank_to(target_ranks[i], target_letters[i]);
                const range_type target{target_letters};
                const auto actual = gv::with_prefilter_packed_windows(
                    bs::make_lazy_sequence_view(query), bs::make_lazy_sequence_view(target), jobs,
                    [&](auto queries, auto targets, const auto& original_lut, auto local_jobs) {
                        auto lut = original_lut;
                        for (std::size_t rank = 0; rank < policy.size(); ++rank)
                            lut.rank_to_rc_code[rank] = policy[rank];
                        return runtime.run_sneaky_snake_rank_batch_roundtrip(
                            queries, targets, lut, local_jobs, validator, range_type::traits::bits_per_char);
                    });
                REQUIRE(actual.size() == expected.size());
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    CAPTURE(sizeof(Word), policy, max_edits, lengths[i]);
                    REQUIRE(same_result(actual[i], expected[i]));
                }
            }
        }
    };
    [&]<std::size_t... Widths>(supported_storage_unit_byte_widths<Widths...>) {
        (exercise.template operator()<
            typename all_storage_unit_byte_widths::template storage_unit<Widths>>(), ...);
    }(all_storage_unit_byte_widths{});
}
