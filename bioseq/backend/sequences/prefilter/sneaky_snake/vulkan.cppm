/**
 * @file vulkan.cppm
 * @brief SneakySnake batch filtering through the Vulkan runtime.
 *
 * The runtime owns GPU buffers and synchronization. This entry point requires
 * a  hardware Vulkan device and reports failure when it is unavailable.
 */
module;

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.prefilter.sneaky_snake.vulkan;

export import bioseq.prefilter.types;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers.vulkan_minimizers;

export namespace bioseq::prefilter {

/**
 * @brief Checks whether a hardware Vulkan device is available for SneakySnake.
 * @return False for software Vulkan devices.
 */
[[nodiscard]] inline bool is_vulkan_sneaky_snake_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/**
 * @brief Runs SneakySnake on a batch of rank-buffer slices using Vulkan.
 * @param query_ranks Query symbols encoded as rank bytes.
 * @param target_ranks Target symbols using the same encoding.
 * @param jobs Slice offsets, lengths, and edit thresholds.
 * @return One result per job in input order
 * @throws bioseq::gpu::vulkan::vulkan_runtime_error If a hardware device is, unavailable
 * or runtime execution fails.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::vector<prefilter_result>
run_sneaky_snake_vulkan_batch(
    bioseq::storage::lazy_sequence_view<p_alphabet> query,
    bioseq::storage::lazy_sequence_view<p_alphabet> target,
    std::span<const prefilter_job> jobs)
{
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(vulkan_runtime, "Vulkan sneaky_snake prefilter");
    return bioseq::gpu::vulkan::with_prefilter_packed_windows(query, target, jobs,
        [&vulkan_runtime](auto queries, auto targets, const auto& rank_lut, auto local_jobs) {
            return vulkan_runtime.run_sneaky_snake_rank_batch_roundtrip(
                queries, targets, rank_lut, local_jobs);
        });
}

} // namespace bioseq::prefilter
