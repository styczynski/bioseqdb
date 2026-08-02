/**
 * @file sneaky_snake_kernel.cppm
 * @brief Canonical generator input for fixed-target SneakySnake SIMD kernels.
 *
 * This module is scanned by bioseq_codegen and is not a final production
 * provider. The generator replaces the annotated `target` namespace with one
 * namespace per compiled Highway target and adds HWY_ATTR to every callable.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module bioseq.highway.sources.prefilter;

import std;
import bioseq.sequences.prefilters.sneaky_snake;

export namespace bioseq::sequences::prefilters {
namespace [[bioseq::simd]] target {

namespace hn = hwy::HWY_NAMESPACE;

namespace sneaky_snake_target_detail {

template <class D>
struct HighwayTraversal final {
    D d;

    [[nodiscard]] std::uint32_t diagonal_run(
        std::span<const std::uint8_t> query,
        std::span<const std::uint8_t> target_ranks,
        std::uint32_t position,
        std::uint32_t submaze_end,
        std::uint32_t query_index) const noexcept
    {
        const hn::Repartition<std::uint8_t, D> byte_d;
        const std::uint32_t target_remaining = submaze_end - position;
        const std::uint32_t query_remaining = static_cast<std::uint32_t>(
            query.size() - static_cast<std::size_t>(query_index));
        const std::uint32_t run_limit = std::min(
            target_remaining,
            query_remaining);

        const std::uint8_t* const query_data = query.data() + query_index;
        const std::uint8_t* const target_data =
            target_ranks.data() + position;
        if (run_limit == 0u || target_data[0] != query_data[0]) {
            return 0u;
        }

        const std::uint32_t lanes = static_cast<std::uint32_t>(
            hn::Lanes(byte_d));
        std::uint32_t run = 1u;
        while (run_limit - run >= 2u * lanes) {
            const auto query_0 = hn::LoadU(byte_d, query_data + run);
            const auto target_0 = hn::LoadU(byte_d, target_data + run);
            const auto query_1 = hn::LoadU(
                byte_d, query_data + run + lanes);
            const auto target_1 = hn::LoadU(
                byte_d, target_data + run + lanes);
            const auto mismatch_0 = hn::Ne(query_0, target_0);
            const auto mismatch_1 = hn::Ne(query_1, target_1);
            if (!hn::AllFalse(byte_d, mismatch_0)) {
                return run + static_cast<std::uint32_t>(
                    hn::FindFirstTrue(byte_d, mismatch_0));
            }
            if (!hn::AllFalse(byte_d, mismatch_1)) {
                return run + lanes + static_cast<std::uint32_t>(
                    hn::FindFirstTrue(byte_d, mismatch_1));
            }
            run += 2u * lanes;
        }
        while (run_limit - run >= lanes) {
            const auto mismatch = hn::Ne(
                hn::LoadU(byte_d, query_data + run),
                hn::LoadU(byte_d, target_data + run));
            if (!hn::AllFalse(byte_d, mismatch)) {
                return run + static_cast<std::uint32_t>(
                    hn::FindFirstTrue(byte_d, mismatch));
            }
            run += lanes;
        }
        while (run < run_limit
               && target_data[run] == query_data[run]) {
            ++run;
        }
        return run;
    }

    template <class Consumer>
    [[nodiscard]] bool visit_candidate_starts(
        std::span<const std::uint8_t> query,
        std::uint32_t filter_offset,
        std::uint8_t filter_rank,
        std::uint32_t position,
        std::uint32_t lower,
        std::uint32_t upper,
        Consumer&& consume) const noexcept
    {
        using ByteD = hn::Repartition<std::uint8_t, D>;
        const ByteD byte_d;
        static_assert(hn::MaxLanes(ByteD{}) <= 64u);
        const std::uint32_t lanes = static_cast<std::uint32_t>(
            hn::Lanes(byte_d));
        const auto wanted = hn::Set(byte_d, filter_rank);
        std::uint32_t candidate = lower;
        while (candidate <= upper
               && upper - candidate + 1u >= lanes) {
            std::uint64_t matches = hn::BitsFromMask(
                byte_d,
                hn::Eq(
                    hn::LoadU(
                        byte_d,
                        query.data() + candidate + filter_offset),
                    wanted));
            while (matches != 0u) {
                const std::uint32_t lane = static_cast<std::uint32_t>(
                    std::countr_zero(matches));
                const std::uint32_t query_start = candidate + lane;
                if (query_start != position && consume(query_start)) {
                    return true;
                }
                matches &= matches - 1u;
            }
            candidate += lanes;
        }

        for (; candidate <= upper; ++candidate) {
            if (candidate != position
                && query[static_cast<std::size_t>(candidate) + filter_offset]
                    == filter_rank
                && consume(candidate)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace sneaky_snake_target_detail

/// Fixed-target batch implementation used by target-local parity tests.
template <class D>
[[nodiscard]] inline std::vector<PrefilterResult>
sneaky_snake_simd_batch_for(
    D d,
    const PrefilterBatchInput& input,
    SneakySnakeOptions options = {})
{
    sneaky_snake_target_detail::HighwayTraversal<D> traversal{.d = d};
    std::vector<PrefilterResult> results(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        const PrefilterJob& job = input.jobs[index];
        if (!prefilter_job_in_bounds(input, job)) {
            results[index] = sneaky_snake_detail::make_result(
                0u, PrefilterStatus::unsupported_conservative, true);
            continue;
        }
        results[index] = sneaky_snake_detail::bounded_with(
            traversal,
            input.query_ranks.subspan(
                job.query_offset, job.query_length),
            input.target_ranks.subspan(
                job.target_offset, job.target_length),
            job.max_edits,
            options);
    }
    return results;
}

/// Exact callable inserted into CommonKernels by each generated target binder.
[[nodiscard]] inline std::vector<PrefilterResult> sneaky_snake_simd_batch(
    const PrefilterBatchInput& input,
    SneakySnakeOptions options = {})
{
    // BitsFromMask returns one uint64_t. Cap scalable targets so every live
    // predicate bit has an exact scalar representation and the traversal does
    // not fail its compile-time width contract on wide SVE/RVV machines.
    const hn::CappedTag<std::uint8_t, 64> d;
    return sneaky_snake_simd_batch_for(d, input, options);
}

} // namespace target
} // namespace bioseq::sequences::prefilters
