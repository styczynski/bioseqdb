/**
 * @file sneaky_snake.cppm
 * @brief Alphabet-generic SneakySnake edit-distance lower-bound prefilter.
 *
 * The primitive consumes already-normalized uint8 rank buffers.  Equal-length
 * jobs run the source SneakySnake maze walk; unequal-length jobs use only the
 * always-safe length-difference lower bound.  The batch ABI intentionally
 * matches the Vulkan job/result layout.
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.sequences.prefilters.sneaky_snake;

export {

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// Production maze geometry shared by the CPU and Vulkan implementations.
inline constexpr std::uint32_t sneaky_snake_default_submaze_width = 512u;
inline constexpr std::uint32_t sneaky_snake_default_max_escape_segments = 512u;

/// Stable wire ordinals shared with the Vulkan result buffer.
enum class SneakySnakeStatus : std::uint32_t {
    success = 0u,
    conservative_unequal = 1u,
    unsupported_conservative = 2u,
};

/// Stable CPU/GPU job ABI. Offsets address the corresponding flat rank buffer.
struct SneakySnakeJob {
    std::uint32_t query_offset = 0u;
    std::uint32_t target_offset = 0u;
    std::uint32_t query_length = 0u;
    std::uint32_t target_length = 0u;
    std::uint32_t max_edits = 0u;
};

/// Stable CPU/GPU result ABI. accepted is encoded as 0 or 1.
struct SneakySnakeResult {
    std::uint32_t lower_bound = 0u;
    std::uint32_t status = static_cast<std::uint32_t>(
        SneakySnakeStatus::success);
    std::uint32_t accepted = 1u;
    std::uint32_t reserved = 0u;
};

static_assert(std::is_standard_layout_v<SneakySnakeJob>);
static_assert(std::is_trivially_copyable_v<SneakySnakeJob>);
static_assert(sizeof(SneakySnakeJob) == 5u * sizeof(std::uint32_t));
static_assert(offsetof(SneakySnakeJob, query_offset) == 0u);
static_assert(offsetof(SneakySnakeJob, target_offset) == 4u);
static_assert(offsetof(SneakySnakeJob, query_length) == 8u);
static_assert(offsetof(SneakySnakeJob, target_length) == 12u);
static_assert(offsetof(SneakySnakeJob, max_edits) == 16u);
static_assert(std::is_standard_layout_v<SneakySnakeResult>);
static_assert(std::is_trivially_copyable_v<SneakySnakeResult>);
static_assert(sizeof(SneakySnakeResult) == 4u * sizeof(std::uint32_t));
static_assert(offsetof(SneakySnakeResult, lower_bound) == 0u);
static_assert(offsetof(SneakySnakeResult, status) == 4u);
static_assert(offsetof(SneakySnakeResult, accepted) == 8u);
static_assert(offsetof(SneakySnakeResult, reserved) == 12u);

struct SneakySnakeOptions {
    /// A zero value selects sneaky_snake_default_submaze_width.
    std::uint32_t submaze_width = sneaky_snake_default_submaze_width;
    /// Zero keeps y equal to the effective submaze width (512 by default).
    std::uint32_t max_escape_segments = 0u;
};

struct SneakySnakeBatchInput {
    std::span<const std::uint8_t> query_ranks{};
    std::span<const std::uint8_t> target_ranks{};
    std::span<const SneakySnakeJob> jobs{};
};

[[nodiscard]] constexpr bool sneaky_snake_evaluated(
    const SneakySnakeResult& result) noexcept
{
    return result.status
        == static_cast<std::uint32_t>(SneakySnakeStatus::success);
}

[[nodiscard]] constexpr bool sneaky_snake_accepted(
    const SneakySnakeResult& result) noexcept
{
    return result.accepted != 0u;
}

namespace sneaky_snake_detail {

[[nodiscard]] constexpr SneakySnakeResult make_result(
    std::uint32_t lower_bound,
    SneakySnakeStatus status,
    bool accepted) noexcept
{
    return SneakySnakeResult{
        .lower_bound = lower_bound,
        .status = static_cast<std::uint32_t>(status),
        .accepted = accepted ? 1u : 0u,
        .reserved = 0u,
    };
}

[[nodiscard]] constexpr std::uint32_t effective_width(
    SneakySnakeOptions options) noexcept
{
    return options.submaze_width == 0u
        ? sneaky_snake_default_submaze_width
        : options.submaze_width;
}

[[nodiscard]] constexpr std::uint32_t effective_escape_limit(
    SneakySnakeOptions options,
    std::uint32_t width) noexcept
{
    return options.max_escape_segments == 0u
        ? width
        : options.max_escape_segments;
}

[[nodiscard]] inline bool slice_is_in_bounds(
    std::span<const std::uint8_t> ranks,
    std::uint32_t offset,
    std::uint32_t length) noexcept
{
    const std::size_t offset_size = static_cast<std::size_t>(offset);
    const std::size_t length_size = static_cast<std::size_t>(length);
    return offset_size <= ranks.size()
        && length_size <= ranks.size() - offset_size;
}

[[nodiscard]] inline std::uint32_t diagonal_run(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::int64_t diagonal) noexcept
{
    const std::int64_t query_start = static_cast<std::int64_t>(position)
        + diagonal;
    if (query_start < 0
        || static_cast<std::uint64_t>(query_start) >= query.size()) {
        return 0u;
    }

    const auto query_index = static_cast<std::uint32_t>(query_start);
    const std::uint32_t target_remaining = submaze_end - position;
    const std::uint32_t query_remaining = static_cast<std::uint32_t>(
        query.size() - static_cast<std::size_t>(query_index));
    const std::uint32_t run_limit = std::min(
        target_remaining,
        query_remaining);

    std::uint32_t run = 0u;
    while (run < run_limit
           && target[static_cast<std::size_t>(position) + run]
               == query[static_cast<std::size_t>(query_index) + run]) {
        ++run;
    }
    return run;
}

[[nodiscard]] inline std::uint32_t longest_diagonal_run(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t max_edits) noexcept
{
    const std::uint32_t remaining = submaze_end - position;
    std::uint32_t best = diagonal_run(
        query, target, position, submaze_end, 0);
    if (best == remaining) {
        return best;
    }

    const std::uint32_t max_shift = std::min(
        max_edits,
        static_cast<std::uint32_t>(query.size()));
    for (std::uint64_t shift = 1u; shift <= max_shift; ++shift) {
        best = std::max(best, diagonal_run(
            query,
            target,
            position,
            submaze_end,
            -static_cast<std::int64_t>(shift)));
        if (best == remaining) {
            break;
        }
        best = std::max(best, diagonal_run(
            query,
            target,
            position,
            submaze_end,
            static_cast<std::int64_t>(shift)));
        if (best == remaining) {
            break;
        }
    }
    return best;
}

[[nodiscard]] inline SneakySnakeResult equal_length_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    if (length == 0u) {
        return make_result(0u, SneakySnakeStatus::success, true);
    }

    const std::uint32_t width = effective_width(options);
    const std::uint32_t escape_limit = effective_escape_limit(options, width);
    std::uint32_t lower_bound = 0u;

    for (std::uint64_t start_wide = 0u; start_wide < length;
         start_wide += width) {
        const std::uint32_t start = static_cast<std::uint32_t>(start_wide);
        const std::uint32_t end = static_cast<std::uint32_t>(std::min(
            static_cast<std::uint64_t>(length),
            start_wide + width));
        std::uint32_t position = start;
        std::uint32_t escapes = 0u;

        while (position < end && escapes < escape_limit) {
            const std::uint32_t best = longest_diagonal_run(
                query, target, position, end, max_edits);
            if (best == end - position) {
                position = end;
                break;
            }

            ++lower_bound;
            if (lower_bound > max_edits) {
                return make_result(
                    lower_bound,
                    SneakySnakeStatus::success,
                    false);
            }
            position += best + 1u;
            ++escapes;
        }
    }

    return make_result(
        lower_bound,
        SneakySnakeStatus::success,
        lower_bound <= max_edits);
}

template <class D>
inline void longest_diagonal_runs_simd(
    D d,
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    const SneakySnakeJob* jobs,
    std::size_t lane_count,
    const std::uint32_t* positions,
    const std::uint32_t* submaze_ends,
    const std::uint32_t* active,
    std::uint32_t* best_runs) noexcept
{
    constexpr std::size_t max_lanes = hn::MaxLanes(D{});
    const auto one = hn::Set(d, 1u);

    std::uint32_t max_shift = 0u;
    for (std::size_t lane = 0u; lane < lane_count; ++lane) {
        best_runs[lane] = 0u;
        if (active[lane] != 0u) {
            max_shift = std::max(
                max_shift,
                std::min(jobs[lane].max_edits, jobs[lane].query_length));
        }
    }
    for (std::size_t lane = lane_count; lane < max_lanes; ++lane) {
        best_runs[lane] = 0u;
    }

    const auto scan_diagonal = [
        d,
        query_ranks,
        target_ranks,
        jobs,
        lane_count,
        positions,
        submaze_ends,
        active,
        best_runs,
        max_lanes,
        one](std::uint32_t shift, bool subtract_shift) {
        HWY_ALIGN std::uint32_t run_lanes[max_lanes]{};
        HWY_ALIGN std::uint32_t query_rank_lanes[max_lanes]{};
        HWY_ALIGN std::uint32_t target_rank_lanes[max_lanes]{};
        HWY_ALIGN std::uint32_t matched_lanes[max_lanes]{};
        bool alive[max_lanes]{};
        auto run_vector = hn::Zero(d);

        for (std::size_t lane = 0u; lane < lane_count; ++lane) {
            alive[lane] = active[lane] != 0u
                && shift <= jobs[lane].max_edits
                && best_runs[lane] < submaze_ends[lane] - positions[lane];
        }

        while (true) {
            bool any_alive = false;
            for (std::size_t lane = 0u; lane < max_lanes; ++lane) {
                // Values outside the uint8 rank domain force inactive lanes to
                // compare unequal without requiring a masked gather.
                query_rank_lanes[lane] = 256u;
                target_rank_lanes[lane] = 257u;
                if (lane >= lane_count || !alive[lane]) {
                    continue;
                }

                const SneakySnakeJob& job = jobs[lane];
                const std::uint64_t target_local
                    = static_cast<std::uint64_t>(positions[lane])
                    + run_lanes[lane];
                const std::int64_t query_local
                    = static_cast<std::int64_t>(positions[lane])
                    + static_cast<std::int64_t>(run_lanes[lane])
                    + (subtract_shift
                        ? -static_cast<std::int64_t>(shift)
                        : static_cast<std::int64_t>(shift));

                if (target_local >= submaze_ends[lane]
                    || query_local < 0
                    || static_cast<std::uint64_t>(query_local)
                        >= job.query_length) {
                    alive[lane] = false;
                    continue;
                }

                const std::size_t query_index
                    = static_cast<std::size_t>(job.query_offset)
                    + static_cast<std::size_t>(query_local);
                const std::size_t target_index
                    = static_cast<std::size_t>(job.target_offset)
                    + static_cast<std::size_t>(target_local);
                query_rank_lanes[lane] = query_ranks[query_index];
                target_rank_lanes[lane] = target_ranks[target_index];
                any_alive = true;
            }

            if (!any_alive) {
                break;
            }

            const auto equal = hn::Eq(
                hn::Load(d, query_rank_lanes),
                hn::Load(d, target_rank_lanes));
            run_vector = hn::IfThenElse(
                equal,
                hn::Add(run_vector, one),
                run_vector);
            hn::Store(run_vector, d, run_lanes);
            hn::Store(hn::VecFromMask(d, equal), d, matched_lanes);

            for (std::size_t lane = 0u; lane < lane_count; ++lane) {
                alive[lane] = alive[lane] && matched_lanes[lane] != 0u;
            }
        }

        hn::Store(
            hn::Max(hn::Load(d, best_runs), run_vector),
            d,
            best_runs);
    };

    for (std::uint64_t shift_wide = 0u; shift_wide <= max_shift;
         ++shift_wide) {
        const auto shift = static_cast<std::uint32_t>(shift_wide);
        scan_diagonal(shift, false);
        if (shift != 0u) {
            scan_diagonal(shift, true);
        }

        bool any_incomplete = false;
        for (std::size_t lane = 0u; lane < lane_count; ++lane) {
            any_incomplete = any_incomplete
                || (active[lane] != 0u
                    && best_runs[lane]
                        < submaze_ends[lane] - positions[lane]);
        }
        if (!any_incomplete) {
            break;
        }
    }
}

} // namespace sneaky_snake_detail

[[nodiscard]] inline bool sneaky_snake_job_in_bounds(
    const SneakySnakeBatchInput& input,
    const SneakySnakeJob& job) noexcept
{
    return sneaky_snake_detail::slice_is_in_bounds(
               input.query_ranks, job.query_offset, job.query_length)
        && sneaky_snake_detail::slice_is_in_bounds(
               input.target_ranks, job.target_offset, job.target_length);
}

/// Scalar single-job entry point over rank spans.
[[nodiscard]] inline SneakySnakeResult sneaky_snake_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options = {}) noexcept
{
    constexpr std::size_t max_u32 = (std::numeric_limits<std::uint32_t>::max)();
    if (query.size() > max_u32 || target.size() > max_u32) {
        return sneaky_snake_detail::make_result(
            0u, SneakySnakeStatus::unsupported_conservative, true);
    }

    const std::uint32_t query_length = static_cast<std::uint32_t>(query.size());
    const std::uint32_t target_length = static_cast<std::uint32_t>(target.size());
    if (query_length != target_length) {
        const std::uint32_t difference = query_length > target_length
            ? query_length - target_length
            : target_length - query_length;
        return sneaky_snake_detail::make_result(
            difference,
            SneakySnakeStatus::conservative_unequal,
            difference <= max_edits);
    }

    return sneaky_snake_detail::equal_length_scalar(
        query, target, max_edits, options);
}

/// Scalar reference batch implementation over flat uint8 rank buffers.
[[nodiscard]] inline std::vector<SneakySnakeResult> sneaky_snake_scalar_batch(
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    std::vector<SneakySnakeResult> results;
    results.reserve(input.jobs.size());
    for (const SneakySnakeJob& job : input.jobs) {
        if (!sneaky_snake_job_in_bounds(input, job)) {
            results.push_back(sneaky_snake_detail::make_result(
                0u, SneakySnakeStatus::unsupported_conservative, true));
            continue;
        }
        results.push_back(sneaky_snake_scalar(
            input.query_ranks.subspan(job.query_offset, job.query_length),
            input.target_ranks.subspan(job.target_offset, job.target_length),
            job.max_edits,
            options));
    }
    return results;
}

/// Highway lane-parallel batch implementation. Each SIMD lane owns one job.
template <class D>
[[nodiscard]] inline std::vector<SneakySnakeResult>
sneaky_snake_simd_batch_for(
    D d,
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    constexpr std::size_t max_lanes = hn::MaxLanes(D{});
    const std::size_t lanes = hn::Lanes(d);
    const std::uint32_t width = sneaky_snake_detail::effective_width(options);
    const std::uint32_t escape_limit
        = sneaky_snake_detail::effective_escape_limit(options, width);
    std::vector<SneakySnakeResult> results(input.jobs.size());

    for (std::size_t first = 0u; first < input.jobs.size(); first += lanes) {
        const std::size_t lane_count = std::min(lanes, input.jobs.size() - first);
        HWY_ALIGN SneakySnakeJob jobs[max_lanes]{};
        HWY_ALIGN std::uint32_t query_lengths[max_lanes]{};
        HWY_ALIGN std::uint32_t target_lengths[max_lanes]{};
        HWY_ALIGN std::uint32_t differences[max_lanes]{};
        HWY_ALIGN std::uint32_t positions[max_lanes]{};
        HWY_ALIGN std::uint32_t submaze_ends[max_lanes]{};
        HWY_ALIGN std::uint32_t escapes[max_lanes]{};
        HWY_ALIGN std::uint32_t lower_bounds[max_lanes]{};
        HWY_ALIGN std::uint32_t max_edits[max_lanes]{};
        HWY_ALIGN std::uint32_t active[max_lanes]{};
        HWY_ALIGN std::uint32_t best_runs[max_lanes]{};
        HWY_ALIGN std::uint32_t remaining[max_lanes]{};

        for (std::size_t lane = 0u; lane < lane_count; ++lane) {
            jobs[lane] = input.jobs[first + lane];
            query_lengths[lane] = jobs[lane].query_length;
            target_lengths[lane] = jobs[lane].target_length;
            max_edits[lane] = jobs[lane].max_edits;
        }

        const auto query_vector = hn::Load(d, query_lengths);
        const auto target_vector = hn::Load(d, target_lengths);
        hn::Store(
            hn::Sub(
                hn::Max(query_vector, target_vector),
                hn::Min(query_vector, target_vector)),
            d,
            differences);

        for (std::size_t lane = 0u; lane < lane_count; ++lane) {
            SneakySnakeResult& result = results[first + lane];
            if (!sneaky_snake_job_in_bounds(input, jobs[lane])) {
                result = sneaky_snake_detail::make_result(
                    0u, SneakySnakeStatus::unsupported_conservative, true);
            } else if (differences[lane] != 0u) {
                result = sneaky_snake_detail::make_result(
                    differences[lane],
                    SneakySnakeStatus::conservative_unequal,
                    differences[lane] <= jobs[lane].max_edits);
            } else if (jobs[lane].query_length == 0u) {
                result = sneaky_snake_detail::make_result(
                    0u, SneakySnakeStatus::success, true);
            } else {
                result = sneaky_snake_detail::make_result(
                    0u, SneakySnakeStatus::success, true);
                active[lane] = 1u;
                submaze_ends[lane] = std::min(
                    jobs[lane].query_length,
                    width);
            }
        }

        while (true) {
            bool any_active = false;
            for (std::size_t lane = 0u; lane < lane_count; ++lane) {
                any_active = any_active || active[lane] != 0u;
            }
            if (!any_active) {
                break;
            }

            sneaky_snake_detail::longest_diagonal_runs_simd(
                d,
                input.query_ranks,
                input.target_ranks,
                jobs,
                lane_count,
                positions,
                submaze_ends,
                active,
                best_runs);

            for (std::size_t lane = 0u; lane < max_lanes; ++lane) {
                remaining[lane] = active[lane] != 0u
                    ? submaze_ends[lane] - positions[lane]
                    : 0u;
            }

            const auto obstacle = hn::Gt(
                hn::Load(d, remaining),
                hn::Load(d, best_runs));
            const auto obstacle_increment = hn::IfThenElse(
                obstacle,
                hn::Set(d, 1u),
                hn::Zero(d));
            const auto updated_lower_bounds = hn::Add(
                hn::Load(d, lower_bounds),
                obstacle_increment);
            const auto updated_positions = hn::IfThenElse(
                obstacle,
                hn::Add(
                    hn::Load(d, positions),
                    hn::Add(hn::Load(d, best_runs), hn::Set(d, 1u))),
                hn::Load(d, submaze_ends));
            const auto updated_escapes = hn::Add(
                hn::Load(d, escapes),
                obstacle_increment);
            hn::Store(updated_lower_bounds, d, lower_bounds);
            hn::Store(updated_positions, d, positions);
            hn::Store(updated_escapes, d, escapes);

            for (std::size_t lane = 0u; lane < lane_count; ++lane) {
                if (active[lane] == 0u) {
                    continue;
                }

                const bool crossed_obstacle = best_runs[lane] < remaining[lane];
                SneakySnakeResult& result = results[first + lane];
                result.lower_bound = lower_bounds[lane];
                if (lower_bounds[lane] > jobs[lane].max_edits) {
                    result.accepted = 0u;
                    active[lane] = 0u;
                    continue;
                }

                if (crossed_obstacle
                    && escapes[lane] >= escape_limit
                    && positions[lane] < submaze_ends[lane]) {
                    positions[lane] = submaze_ends[lane];
                }

                if (positions[lane] >= submaze_ends[lane]) {
                    if (submaze_ends[lane] >= jobs[lane].query_length) {
                        result.accepted = 1u;
                        active[lane] = 0u;
                    } else {
                        positions[lane] = submaze_ends[lane];
                        submaze_ends[lane] = static_cast<std::uint32_t>(
                            std::min(
                                static_cast<std::uint64_t>(
                                    jobs[lane].query_length),
                                static_cast<std::uint64_t>(positions[lane])
                                    + width));
                        escapes[lane] = 0u;
                    }
                }
            }
        }
    }

    return results;
}

/// Active-target Highway entry point used by the pipeline and benchmarks.
[[nodiscard]] inline std::vector<SneakySnakeResult> sneaky_snake_simd_batch(
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    const hn::ScalableTag<std::uint32_t> d;
    return sneaky_snake_simd_batch_for(d, input, options);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export
