/**
 * @file vulkan.cppm
 * @brief GateKeeper batch filtering theough the Vulkan runtime.
 *
 * The runtime owns GPU buffers and synchronization. This, entry point requires
 * a hardware Vulkan device and reports failure when it is unavailable.
 */
module;

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.prefilter.gatekeeper.vulkan;

export import bioseq.prefilter.types;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.simd.utils;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers.vulkan_minimizers;

export namespace bioseq::prefilter {

/**
 * @brief Checks whether a hardware Vulkan device is available for GateKeeper.
 * @return False for software Vulkan devices.
 */
[[nodiscard]] inline bool is_vulkan_gatekeeper_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/**
 * @brief Runs GateKeeper on a batch of rank-buffer slices using Vulkan.
 * @param query_ranks Query symbols encoded as rank bytes.
 * @param target_ranks Target symbols using the same encoding.
 * @param jobs Slice offsets, lengths, and edit thresholds.
 * @param validate_ranks Caller-selected CPU kernel checking all uploaded native ranks.
 * @return One result per job in  input order.
 * @throws bioseq::gpu::vulkan::vulkan_runtime_error If a hardware device is unavailable
 * or runtime execution fails.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::vector<prefilter_result>
run_gatekeeper_vulkan_batch(
    bioseq::storage::lazy_sequence_view<p_alphabet> query,
    bioseq::storage::lazy_sequence_view<p_alphabet> target,
    std::span<const prefilter_job> jobs,
    bioseq::simd::sequence_packed_rank_validation_kernel validate_ranks)
{
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(vulkan_runtime, "Vulkan gatekeeper prefilter");
    return bioseq::gpu::vulkan::with_prefilter_packed_windows(query, target, jobs,
        [&vulkan_runtime, validate_ranks](auto queries, auto targets, const auto& rank_lut, auto local_jobs) {
            return vulkan_runtime.run_gatekeeper_rank_batch_roundtrip(
                queries, targets, rank_lut, local_jobs, validate_ranks);
        });
}

} // namespace bioseq::prefilter
