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
 * Failure policy: if this backend is selected, Vulkan execution is mandatory.
 * `vulkan_enabled == false`, an unusable device, unsupported shape,
 * memory-admission failure, or any runtime error throws `VulkanRuntimeError`.
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

export module bioseq.sequences.chaining.vulkan;

import bioseq.common.mlogging;
import bioseq.common.context;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.scalar;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.parallelism.memory_admission;
import bioseq.instr;

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
    return bioseq::gpu::vulkan::choose_chaining_variant(
        bioseq::gpu::vulkan::VulkanStaticVariantInputs{
            .max_workgroup_invocations = f.max_workgroup_invocations,
            .max_shared_memory_bytes = f.max_shared_memory_bytes,
            .default_window_size = 10u,
        });
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

constexpr std::uint32_t kGpuChainCompactRangeThreshold = 131'072u;
constexpr std::uint32_t kGpuChainCompactRangeTarget = 65'536u;
constexpr std::size_t kCoordinateOnlyFastProbeLimit = 16u;

/// Very long hard ranges are usually near-colinear dense minimizer runs where
/// chaining every anchor adds DP cost without improving the downstream extension
/// seed. Keep sorted order and both range endpoints, but adaptively thin the
/// interior so the GPU DP critical path is bounded.
[[nodiscard]] inline bool compact_long_gpu_chain_ranges(
    std::span<const Anchor> anchors,
    std::span<const bioseq::gpu::vulkan::GpuChainRange> ranges,
    ArenaSlab<Anchor>& compacted_anchors,
    std::span<bioseq::gpu::vulkan::GpuChainRange> compacted_ranges)
{
    bool needs_compaction = false;
    std::size_t reserve_count = 0;
    for (const auto& range : ranges) {
        if (range.length > kGpuChainCompactRangeThreshold) {
            needs_compaction = true;
            const std::uint32_t stride =
                (range.length + kGpuChainCompactRangeTarget - 1u)
                / kGpuChainCompactRangeTarget;
            reserve_count += (range.length + stride - 1u) / stride + 1u;
        } else {
            reserve_count += range.length;
        }
    }
    if (!needs_compaction) return false;

    compacted_anchors.reserve(reserve_count);
    std::size_t out_range_count = 0;
    for (const auto& range : ranges) {
        const std::uint32_t new_begin =
            static_cast<std::uint32_t>(compacted_anchors.size());
        const std::uint32_t stride = range.length > kGpuChainCompactRangeThreshold
            ? (range.length + kGpuChainCompactRangeTarget - 1u)
                / kGpuChainCompactRangeTarget
            : 1u;
        for (std::uint32_t i = 0; i < range.length; ++i) {
            if ((i % stride) == 0u || i + 1u == range.length) {
                compacted_anchors.push_back(anchors[range.begin + i]);
            }
        }
        compacted_ranges[out_range_count++] = bioseq::gpu::vulkan::GpuChainRange{
            .begin = new_begin,
            .length = static_cast<std::uint32_t>(
                compacted_anchors.size() - new_begin),
        };
    }
    return true;
}

/// Emit the highest-scoring predecessor trace only. This is selected only when
/// SQL projection flags prove that downstream code needs a coordinate/strand
/// mapping but not CIGAR, divergence, MAPQ, or the full secondary list.
[[nodiscard]] inline bool backtrack_best_coordinate_chain(
    std::span<const Anchor> anchors,
    std::span<const int32_t> best_scores,
    std::span<const int64_t> predecessors,
    int32_t min_score,
    int32_t min_seed_count,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    const std::size_t anchor_count = anchors.size();
    if (anchor_count == 0) return false;

    auto& bt_ctx = context::current();
    auto visited = bt_ctx.alloc_chain_array<std::uint8_t>(anchor_count);
    std::memset(visited.data(), 0, anchor_count * sizeof(std::uint8_t));

    for (std::size_t attempt = 0; attempt < kCoordinateOnlyFastProbeLimit; ++attempt) {
        int32_t best_score = min_score - 1;
        int64_t best_anchor = -1;
        for (std::size_t i = 0; i < anchor_count; ++i) {
            if (visited[i] == 0 && best_scores[i] >= best_score) {
                best_score = best_scores[i];
                best_anchor = static_cast<int64_t>(i);
            }
        }
        if (best_anchor < 0 || best_score < min_score) return false;

        std::size_t chain_len = 0;
        int64_t stop_idx = -1;
        for (int64_t trace_idx = best_anchor; trace_idx >= 0; trace_idx = predecessors[trace_idx])
        {
            if (visited[static_cast<std::size_t>(trace_idx)] != 0) {
                stop_idx = trace_idx;
                break;
            }
            ++chain_len;
        }
        const int32_t candidate_score = stop_idx < 0
            ? best_score
            : best_score - best_scores[static_cast<std::size_t>(stop_idx)];

        if (candidate_score >= min_score
            && chain_len >= static_cast<std::size_t>(min_seed_count)) {
            auto trace = bt_ctx.alloc_chain_array<int64_t>(chain_len);
            std::size_t trace_count = 0;
            for (int64_t trace_idx = best_anchor;
                 trace_idx >= 0;
                 trace_idx = predecessors[trace_idx])
            {
                if (visited[static_cast<std::size_t>(trace_idx)] != 0) break;
                trace[trace_count++] = trace_idx;
            }

            const std::size_t anchor_start = reordered_anchors_out.size();
            reordered_anchors_out.reserve(anchor_start + trace_count);
            for (std::size_t i = trace_count; i > 0; --i) {
                reordered_anchors_out.push_back(
                    anchors[static_cast<std::size_t>(trace[i - 1])]);
            }

            chains_out.push_back(Chain{
                .score = candidate_score,
                .anchor_count = static_cast<int32_t>(trace_count),
                .anchor_start = static_cast<int32_t>(anchor_start),
            });
            return true;
        }

        for (int64_t trace_idx = best_anchor; trace_idx >= 0; trace_idx = predecessors[trace_idx])
        {
            if (visited[static_cast<std::size_t>(trace_idx)] != 0) break;
            visited[static_cast<std::size_t>(trace_idx)] = 1u;
        }
    }

    return false;
}

/// Run the Vulkan tiled-DP chaining path. Throws `VulkanRuntimeError` on any
/// selected-GPU decline; algorithm fallback must be chosen by the caller before
/// entering the pipeline.
inline void run_chain_anchors_vulkan_impl(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    const std::size_t n = anchors.size();
    const std::uint64_t n64 = static_cast<std::uint64_t>(n);
    if (n == 0) return;
    if (!params.vulkan_enabled) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "Vulkan chaining selected but Minimap2Params disabled Vulkan");
    }
    if (n64 > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but anchor count {} exceeds uint32_t", n64));
    }

    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    if (!runtime.usable()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but runtime is unavailable: {}",
            runtime.initialization_error()));
    }

    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    if (max_dist_x < params.bandwidth) max_dist_x = params.bandwidth;
    if (max_dist_y < params.bandwidth) max_dist_y = params.bandwidth;

    const auto key = choose_vulkan_chaining_pipeline(runtime.features());

    const std::size_t range_count =
        count_gpu_chain_ranges(anchors, static_cast<std::uint64_t>(max_dist_x));
    if (range_count == 0) return;
    // DOC0353: the chaining kernel grid-strides one invocation per range and the
    // runtime clamps the dispatch to maxComputeWorkGroupCount[0], so range_count
    // is no longer bounded by the workgroup count — it is just the data size.

    const std::uint64_t reserved_bytes =
        bioseq::gpu::vulkan::chaining_reserved_bytes(n64, range_count);
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reserved_bytes);
    if (!permit) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but memory admission failed for {} bytes",
            reserved_bytes));
    }

    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto& dp_ctx = context::current();
    auto ranges = build_gpu_chain_ranges(
        anchors,
        static_cast<std::uint64_t>(max_dist_x),
        range_count);

    std::span<const Anchor> dp_anchors = anchors;
    std::span<const bioseq::gpu::vulkan::GpuChainRange> dp_ranges = ranges;
    ArenaSlab<Anchor> compacted_anchors = make_arena_slab<Anchor>();
    auto compacted_ranges = dp_ctx.alloc_chain_array<bioseq::gpu::vulkan::GpuChainRange>(
        ranges.size());
    if (compact_long_gpu_chain_ranges(
            anchors, ranges, compacted_anchors, compacted_ranges)) {
        dp_anchors = compacted_anchors.as_const_span();
        dp_ranges = {
            compacted_ranges.data(),
            ranges.size(),
        };
    }

    const std::size_t dp_n = dp_anchors.size();
    const std::uint64_t dp_n64 = static_cast<std::uint64_t>(dp_n);
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(static_cast<std::int64_t>(dp_n));
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(static_cast<std::int64_t>(dp_n));
    for (std::size_t i = 0; i < dp_n; ++i) {
        const auto span_score = static_cast<int32_t>(dp_anchors[i].query_span);
        best_scores[i] = span_score;
        predecessors[i] = -1;
    }

    const std::uint64_t scratch_bytes =
        bioseq::gpu::vulkan::chaining_transfer_scratch_bytes(dp_n64, dp_ranges.size());
    auto transfer_scratch = runtime.create_host_buffer(
        scratch_bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto anchors_buf = runtime.create_device_buffer(
        dp_n64 * sizeof(Anchor),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto ranges_buf = runtime.create_device_buffer(
        dp_ranges.size() * sizeof(bioseq::gpu::vulkan::GpuChainRange),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto score_buf = runtime.create_device_buffer(
        dp_n64 * sizeof(int32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto pred_buf = runtime.create_device_buffer(
        dp_n64 * sizeof(int64_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const std::int32_t gpu_max_chain_iter =
        std::min<std::int32_t>(params.max_chain_iter, 3000);

    if (params.coordinate_only) {
        auto coord_trace = runtime.run_chaining_coordinate_trace_roundtrip(
            key,
            transfer_scratch,
            anchors_buf,
            ranges_buf,
            score_buf,
            pred_buf,
            std::as_bytes(std::span<const Anchor>{dp_anchors.data(), dp_anchors.size()}),
            std::as_bytes(std::span<const bioseq::gpu::vulkan::GpuChainRange>{
                dp_ranges.data(), dp_ranges.size()}),
            std::as_bytes(std::span<const int32_t>{best_scores.data(), best_scores.size()}),
            std::as_bytes(std::span<const int64_t>{predecessors.data(), predecessors.size()}),
            static_cast<std::uint32_t>(dp_n64),
            static_cast<std::uint32_t>(dp_ranges.size()),
            static_cast<std::uint64_t>(params.query_len),
            max_dist_x,
            max_dist_y,
            params.bandwidth,
            gpu_max_chain_iter,
            params.gap_penalty,
            params.skip_penalty,
            params.max_chain_skip,
            params.min_chain_score,
            params.min_seed_count,
            kCoordinateOnlyFastProbeLimit);
        if (coord_trace.result.valid == 0u) {
            return;
        }
        const std::size_t trace_count = coord_trace.trace.size();
        if (trace_count == 0
            || trace_count > static_cast<std::size_t>(
                (std::numeric_limits<int32_t>::max)())) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan coordinate-only trace has invalid length");
        }

        const std::size_t anchor_start = reordered_anchors_out.size();
        reordered_anchors_out.reserve(anchor_start + trace_count);
        for (std::size_t i = trace_count; i > 0; --i) {
            const std::uint32_t anchor_idx = coord_trace.trace[i - 1];
            if (anchor_idx >= dp_n) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(
                    "Vulkan coordinate-only trace contains invalid anchor index");
            }
            reordered_anchors_out.push_back(dp_anchors[anchor_idx]);
        }
        chains_out.push_back(Chain{
            .score = coord_trace.result.chain_score,
            .anchor_count = static_cast<int32_t>(trace_count),
            .anchor_start = static_cast<int32_t>(anchor_start),
        });
        return;
    }

    runtime.run_chaining_tiled_dp_roundtrip(
        key,
        transfer_scratch,
        anchors_buf,
        ranges_buf,
        score_buf,
        pred_buf,
        std::as_bytes(std::span<const Anchor>{dp_anchors.data(), dp_anchors.size()}),
        std::as_bytes(std::span<const bioseq::gpu::vulkan::GpuChainRange>{
            dp_ranges.data(), dp_ranges.size()}),
        std::as_writable_bytes(std::span<int32_t>{best_scores.data(), best_scores.size()}),
        std::as_writable_bytes(std::span<int64_t>{predecessors.data(), predecessors.size()}),
        static_cast<std::uint32_t>(dp_n64),
        static_cast<std::uint32_t>(dp_ranges.size()),
        max_dist_x,
        max_dist_y,
        params.bandwidth,
        gpu_max_chain_iter,
        params.gap_penalty,
        params.skip_penalty,
        params.max_chain_skip);

    for (std::size_t i = 0; i < dp_n; ++i) {
        const int64_t pred = predecessors[i];
        if (pred < -1 || pred >= static_cast<int64_t>(i)) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan chaining produced an invalid predecessor");
        }
        if (best_scores[i] < static_cast<int32_t>(dp_anchors[i].query_span)) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan chaining produced a score below the anchor span");
        }
    }

    backtrack_chains(
        dp_anchors,
        std::span<const int32_t>{best_scores.data(), best_scores.size()},
        std::span<const int64_t>{predecessors.data(), predecessors.size()},
        std::span<const int32_t>{},
        params.min_chain_score,
        params.min_seed_count,
        chains_out,
        reordered_anchors_out);

}

}  // namespace detail

/// Public entry point. Pins the `Anchor` ABI and runs the Vulkan tiled-DP path.
/// If this backend is selected, no scalar fallback is allowed here.
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

    detail::run_chain_anchors_vulkan_impl(
        anchors, params, chains_out, reordered_anchors_out);
}

}  // namespace bioseq::chaining
