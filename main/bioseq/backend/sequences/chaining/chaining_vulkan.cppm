/**
 * @file chaining_vulkan.cppm
 * @brief Phase A Vulkan (G3SA tiled-DP) anchor-chaining backend.
 *
 * Consumes the anchors `collect_seed_hits` already produced and sorted, runs
 * the G3SA-style tiled dynamic-programming score generation on Vulkan, then
 * reuses bioseq's `backtrack_chains` to emit the same `(Chain, reordered
 * Anchor)` ABI the downstream stages already consume. The shader returns the DP
 * arrays (`best_scores`, absolute `predecessors`); this module never ports
 * G3SA's CUDA backtracking stack.
 *
 * Alphabet-neutral by construction: chaining reads anchor coordinates and
 * `query_span` only, never sequence letters, so the same backend serves dna4 /
 * dna5 / any other alphabet (DOC0304 §8).
 *
 * Failure policy (DOC0304 §0, §4.1): `vulkan_enabled == false`, an unusable
 * device, too many anchors/ranges for Phase A, memory-admission failure, or any
 * `VulkanRuntimeError` falls back to `chain_anchors_scalar` BEFORE the output
 * slabs are mutated.
 */
module;

#include "common.h"

#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include <vulkan/vulkan.h>

export module bioseq_sequences.chaining.vulkan;

import mlogging;
import context;
import bioseq_sequences.chaining.utils;
import bioseq_sequences.chaining.scalar;
import bioseq_gpu.vulkan.runtime;
import bioseq_parallelism.memory_admission;
import bioseq_instr;

export namespace bioseq::chaining {

/// True when a usable Vulkan device exists for Phase A chaining. The GPU
/// invariant / smoke tests skip cleanly when this is false.
[[nodiscard]] inline bool vulkan_chaining_supported() {
    return bioseq::gpu::vulkan::Runtime::instance().usable();
}

/// Pick the tile/local workgroup dimensions for the chaining pipeline from the
/// device limits. 384 is the portable floor (~12 KiB shared stays under the
/// 16 KiB Vulkan guaranteed minimum); 1024 is only chosen on devices that can
/// host G3SA's original tile footprint comfortably.
[[nodiscard]] inline bioseq::gpu::vulkan::ChainingPipelineKey
choose_vulkan_chaining_pipeline(const bioseq::gpu::vulkan::VulkanFeatureSet& f) noexcept {
    bioseq::gpu::vulkan::ChainingPipelineKey key;

    if (f.max_workgroup_invocations >= 1024 && f.max_shared_memory_bytes >= 40 * 1024) {
        key.tile_size = 1024;
        key.local_size = 256;  // local invocations loop over the tile
    } else if (f.max_workgroup_invocations >= 256 && f.max_shared_memory_bytes >= 24 * 1024) {
        key.tile_size = 512;
        key.local_size = 128;
    } else {
        key.tile_size = 384;
        key.local_size = 128;
    }
    return key;
}

namespace detail {

/// Count the independent hard chaining ranges in a sorted anchor span. A range
/// boundary appears where the strand/chromosome key changes or two adjacent
/// reference positions differ by more than `max_ref_dist` (anchors cannot chain
/// across such a gap, so each range is an independent workgroup).
[[nodiscard]] inline std::size_t
count_gpu_chain_ranges(std::span<const Anchor> anchors, std::uint64_t max_ref_dist)
{
    if (anchors.empty()) return 0;
    std::size_t range_count = 1;
    for (std::size_t i = 1; i < anchors.size(); ++i) {
        const Anchor& prev = anchors[i - 1];
        const Anchor& cur = anchors[i];
        const bool key_changed =
            cur.ref_chromosome_key() != prev.ref_chromosome_key();
        const bool hard_ref_gap =
            cur.ref_position > prev.ref_position &&
            cur.ref_position - prev.ref_position > max_ref_dist;
        if (key_changed || hard_ref_gap) {
            ++range_count;
        }
    }
    return range_count;
}

/// Materialise the hard chaining ranges into a chain-arena array. The boundary
/// predicate matches `count_gpu_chain_ranges` exactly so the fill count equals
/// the pre-counted `expected_range_count`.
[[nodiscard]] inline std::span<bioseq::gpu::vulkan::GpuChainRange>
build_gpu_chain_ranges(
    std::span<const Anchor> anchors,
    std::uint64_t max_ref_dist,
    std::size_t expected_range_count)
{
    auto& ctx = context::current();
    auto ranges = ctx.alloc_chain_array<bioseq::gpu::vulkan::GpuChainRange>(
        expected_range_count);
    std::size_t range_count = 0;
    if (anchors.empty()) {
        return {ranges.data(), 0};
    }

    std::size_t begin = 0;
    for (std::size_t i = 1; i < anchors.size(); ++i) {
        const Anchor& prev = anchors[i - 1];
        const Anchor& cur = anchors[i];
        const bool key_changed =
            cur.ref_chromosome_key() != prev.ref_chromosome_key();
        const bool hard_ref_gap =
            cur.ref_position > prev.ref_position &&
            cur.ref_position - prev.ref_position > max_ref_dist;
        if (key_changed || hard_ref_gap) {
            ranges[range_count++] = bioseq::gpu::vulkan::GpuChainRange{
                .begin = static_cast<std::uint32_t>(begin),
                .length = static_cast<std::uint32_t>(i - begin),
            };
            begin = i;
        }
    }
    ranges[range_count++] = bioseq::gpu::vulkan::GpuChainRange{
        .begin = static_cast<std::uint32_t>(begin),
        .length = static_cast<std::uint32_t>(anchors.size() - begin),
    };
    DEBUG_ASSERT(range_count == expected_range_count,
        "GPU chain range count/fill mismatch");
    return {ranges.data(), range_count};
}

/// Attempt the Vulkan tiled-DP chaining path. Returns true if it produced the
/// chains (output slabs written by `backtrack_chains`); returns false to
/// request CPU fallback; throws `VulkanRuntimeError` on a GPU failure (caught by
/// `chain_anchors_vulkan` before any output mutation).
[[nodiscard]] inline bool try_chain_anchors_vulkan_impl(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    const std::size_t n = anchors.size();
    const std::uint64_t n64 = static_cast<std::uint64_t>(n);
    if (n == 0) return true;
    if (!params.vulkan_enabled) {
        bioseq_log<DBG>("[chaining_vulkan] Minimap2Params disabled Vulkan; CPU fallback");
        return false;
    }
    if (n64 > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        bioseq_log<DBG>("[chaining_vulkan] too many anchors for Phase A Vulkan path: {}", n64);
        return false;
    }

    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    if (!runtime.usable()) {
        return false;
    }

    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    if (max_dist_x < params.bandwidth) max_dist_x = params.bandwidth;
    if (max_dist_y < params.bandwidth) max_dist_y = params.bandwidth;

    const auto key = choose_vulkan_chaining_pipeline(runtime.features());

    const std::size_t range_count =
        count_gpu_chain_ranges(anchors, static_cast<std::uint64_t>(max_dist_x));
    if (range_count == 0) return true;
    if (range_count > runtime.features().max_workgroup_count_x) {
        bioseq_log<DBG>(
            "[chaining_vulkan] {} ranges exceed maxComputeWorkGroupCount[0]={}; CPU fallback",
            range_count, runtime.features().max_workgroup_count_x);
        return false;
    }

    const std::uint64_t reserved_bytes =
        bioseq::gpu::vulkan::chaining_reserved_bytes(n64, range_count);
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reserved_bytes);
    if (!permit) {
        bioseq_log<DBG>(
            "[chaining_vulkan] memory admission failed for {} bytes; CPU fallback",
            reserved_bytes);
        return false;
    }

    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto& dp_ctx = context::current();
    const auto ranges = build_gpu_chain_ranges(
        anchors,
        static_cast<std::uint64_t>(max_dist_x),
        range_count);
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(static_cast<std::int64_t>(n));
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(static_cast<std::int64_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const auto span_score = static_cast<int32_t>(anchors[i].query_span);
        best_scores[i] = span_score;
        predecessors[i] = -1;
    }

    const std::uint64_t scratch_bytes =
        bioseq::gpu::vulkan::chaining_transfer_scratch_bytes(n64, ranges.size());
    auto transfer_scratch = runtime.create_host_buffer(
        scratch_bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto anchors_buf = runtime.create_device_buffer(
        n64 * sizeof(Anchor),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto ranges_buf = runtime.create_device_buffer(
        ranges.size() * sizeof(bioseq::gpu::vulkan::GpuChainRange),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto score_buf = runtime.create_device_buffer(
        n64 * sizeof(int32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto pred_buf = runtime.create_device_buffer(
        n64 * sizeof(int64_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    runtime.run_chaining_tiled_dp_roundtrip(
        key,
        transfer_scratch,
        anchors_buf,
        ranges_buf,
        score_buf,
        pred_buf,
        std::as_bytes(std::span<const Anchor>{anchors.data(), anchors.size()}),
        std::as_bytes(std::span<const bioseq::gpu::vulkan::GpuChainRange>{
            ranges.data(), ranges.size()}),
        std::as_writable_bytes(std::span<int32_t>{best_scores.data(), best_scores.size()}),
        std::as_writable_bytes(std::span<int64_t>{predecessors.data(), predecessors.size()}),
        static_cast<std::uint32_t>(n64),
        static_cast<std::uint32_t>(ranges.size()),
        max_dist_x,
        max_dist_y,
        params.bandwidth,
        params.max_chain_iter,
        params.gap_penalty,
        params.skip_penalty);

    for (std::size_t i = 0; i < n; ++i) {
        const int64_t pred = predecessors[i];
        if (pred < -1 || pred >= static_cast<int64_t>(i)) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan chaining produced an invalid predecessor");
        }
        if (best_scores[i] < static_cast<int32_t>(anchors[i].query_span)) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan chaining produced a score below the anchor span");
        }
    }

    backtrack_chains(
        anchors,
        std::span<const int32_t>{best_scores.data(), best_scores.size()},
        std::span<const int64_t>{predecessors.data(), predecessors.size()},
        std::span<const int32_t>{},
        params.min_chain_score,
        params.min_seed_count,
        chains_out,
        reordered_anchors_out);

    return true;
}

}  // namespace detail

/// Public entry point. Pins the `Anchor` ABI with static asserts, runs the
/// Vulkan tiled-DP path, and falls back to `chain_anchors_scalar` if it returns
/// false or throws — the fallback runs before `backtrack_chains` mutates the
/// output slabs, so the public chaining call is always safe.
inline void chain_anchors_vulkan(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    static_assert(sizeof(Anchor) == 24);
    static_assert(offsetof(Anchor, ref_position) == 0);
    static_assert(offsetof(Anchor, query_position) == 8);
    static_assert(offsetof(Anchor, is_reverse) == 16);
    static_assert(offsetof(Anchor, query_span) == 20);
    static_assert(offsetof(Anchor, flags) == 21);
    static_assert(offsetof(Anchor, segment_id) == 22);
    static_assert(std::endian::native == std::endian::little);

    static const auto* _desc =
        bioseq::instr::register_region_static("chain_anchors_vulkan");
    auto _scope = bioseq::instr::region(_desc);

    try {
        if (detail::try_chain_anchors_vulkan_impl(
                anchors, params, chains_out, reordered_anchors_out))
        {
            return;
        }
    } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& e) {
        bioseq_log<DBG>("[chaining_vulkan] Vulkan failure; CPU fallback: {}", e.what());
    }
    chain_anchors_scalar(anchors, params, chains_out, reordered_anchors_out);
}

}  // namespace bioseq::chaining
