/**
 * @file vulkan/abi/prefilter.cppm
 * @brief GPU prefilter limits and parameters shared with the shaders.
 */
module;

#include <cstddef>

export module bioseq.gpu.vulkan.abi:prefilter;

import std;
import bioseq.prefilter.types;

export namespace bioseq::gpu::vulkan {

/** @brief Maximum symbols in one SneakySnake submaze. */
inline constexpr std::uint32_t k_gpu_sneaky_snake_submaze_length = 512u;
/** @brief Largest supported SneakySnake edit threshold. */
inline constexpr std::uint32_t k_gpu_sneaky_snake_max_edits = 31u;
/** @brief Longest sequence accepted by the generic GateKeeper kernel. */
inline constexpr std::uint32_t k_gpu_gatekeeper_max_length = 128u;
/** @brief Highest inclusive edit threshold handled by GateKeeper. */
inline constexpr std::uint32_t k_gpu_gatekeeper_max_edits = 8u;
/**
 * @brief Parameters sent to a GPU prefilter dispatch.
 *
 * Counts measure sequence symbols, excluding buffer padding. The 16-byte
 * layout and field order must match the GLSL push-constant block.
 */
struct gpu_prefilter_push {
    std::uint32_t job_count = 0; ///< Number of jobs and matching result slots.
    std::uint32_t mode = 0;      ///< SneakySnake lanes per job (1 or 8). GateKeeper uses 0.
    std::uint32_t query_rank_count = 0;  ///< Query symbol count used to check job ranges.
    std::uint32_t target_rank_count = 0; ///< Target symbol count used to check job ranges.
};
// Keep the C++ job layout equal to the shader job layout.
static_assert(std::is_standard_layout_v<bioseq::prefilter::prefilter_job>);
static_assert(sizeof(bioseq::prefilter::prefilter_job) == 20);
static_assert(offsetof(bioseq::prefilter::prefilter_job, query_offset) == 0);
static_assert(offsetof(bioseq::prefilter::prefilter_job, target_offset) == 4);
static_assert(offsetof(bioseq::prefilter::prefilter_job, query_length) == 8);
static_assert(offsetof(bioseq::prefilter::prefilter_job, target_length) == 12);
static_assert(offsetof(bioseq::prefilter::prefilter_job, max_edits) == 16);
// Keep the C++ result layout equal to the shader result layout.
static_assert(std::is_standard_layout_v<bioseq::prefilter::prefilter_result>);
static_assert(sizeof(bioseq::prefilter::prefilter_result) == 16);
static_assert(offsetof(bioseq::prefilter::prefilter_result, lower_bound) == 0);
static_assert(offsetof(bioseq::prefilter::prefilter_result, status) == 4);
static_assert(offsetof(bioseq::prefilter::prefilter_result, is_accepted) == 8);
static_assert(offsetof(bioseq::prefilter::prefilter_result, diagnostics) == 12);
// Keep the push-constant layout equal to the GLSL block.
static_assert(std::is_standard_layout_v<gpu_prefilter_push>);
static_assert(sizeof(gpu_prefilter_push) == 16);
static_assert(offsetof(gpu_prefilter_push, job_count) == 0);
static_assert(offsetof(gpu_prefilter_push, mode) == 4);
static_assert(offsetof(gpu_prefilter_push, query_rank_count) == 8);
static_assert(offsetof(gpu_prefilter_push, target_rank_count) == 12);

} // namespace bioseq::gpu::vulkan
