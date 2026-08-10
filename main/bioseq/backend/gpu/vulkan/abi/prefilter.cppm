/**
 * @file
 * @brief Defines Vulkan rank-prefilter limits and reflected push constants.
 */
module;

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.gpu.vulkan.abi:prefilter;

import bioseq.prefilter.types;

export namespace bioseq::gpu::vulkan {

/** @brief Base span processed by one SneakySnake submaze. */
inline constexpr std::uint32_t k_gpu_sneaky_snake_submaze_length = 512u;
/** @brief Maximum edit threshold represented by SneakySnake masks. */
inline constexpr std::uint32_t k_gpu_sneaky_snake_max_edits = 31u;
/** @brief Maximum sequence length handled by GateKeeper. */
inline constexpr std::uint32_t k_gpu_gatekeeper_max_length = 128u;
/** @brief Maximum edit threshold handled by GateKeeper. */
inline constexpr std::uint32_t k_gpu_gatekeeper_max_edits = 8u;
/** @brief Carries rank-prefilter batch mode, count, and buffer bounds. */
struct gpu_prefilter_push {
    std::uint32_t job_count = 0;
    std::uint32_t mode = 0;
    std::uint32_t query_rank_count = 0;
    std::uint32_t target_rank_count = 0;
};
static_assert(std::is_standard_layout_v<
    bioseq::prefilter::prefilter_job>);
static_assert(sizeof(bioseq::prefilter::prefilter_job) == 20);
static_assert(offsetof(
    bioseq::prefilter::prefilter_job, query_offset) == 0);
static_assert(offsetof(
    bioseq::prefilter::prefilter_job, target_offset) == 4);
static_assert(offsetof(
    bioseq::prefilter::prefilter_job, query_length) == 8);
static_assert(offsetof(
    bioseq::prefilter::prefilter_job, target_length) == 12);
static_assert(offsetof(
    bioseq::prefilter::prefilter_job, max_edits) == 16);
static_assert(std::is_standard_layout_v<
    bioseq::prefilter::prefilter_result>);
static_assert(sizeof(bioseq::prefilter::prefilter_result) == 16);
static_assert(offsetof(
    bioseq::prefilter::prefilter_result, lower_bound) == 0);
static_assert(offsetof(
    bioseq::prefilter::prefilter_result, status) == 4);
static_assert(offsetof(
    bioseq::prefilter::prefilter_result, is_accepted) == 8);
static_assert(offsetof(
    bioseq::prefilter::prefilter_result, diagnostics) == 12);
static_assert(std::is_standard_layout_v<gpu_prefilter_push>);
static_assert(sizeof(gpu_prefilter_push) == 16);
static_assert(offsetof(gpu_prefilter_push, job_count) == 0);
static_assert(offsetof(gpu_prefilter_push, mode) == 4);
static_assert(offsetof(gpu_prefilter_push, query_rank_count) == 8);
static_assert(offsetof(gpu_prefilter_push, target_rank_count) == 12);

} // namespace bioseq::gpu::vulkan
