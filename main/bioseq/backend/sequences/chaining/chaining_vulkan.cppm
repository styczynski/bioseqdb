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
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    return runtime.usable() && !runtime.features().software_device;
}

/// Throws when the selected Vulkan chaining backend is not allowed to execute.
/// This is intentionally not a fallback hook: explicit GPU selection must fail
/// loudly instead of routing work through CPU kernels.
inline void require_vulkan_chaining_supported() {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    if (!runtime.usable()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but runtime is unavailable: {}",
            runtime.initialization_error()));
    }
    if (runtime.features().software_device) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but the detected Vulkan device '{}' is "
            "a CPU/software device (VkPhysicalDeviceType={}); Linux software "
            "Vulkan chaining is not trusted. Select a CPU chaining algorithm "
            "explicitly or run on a supported hardware Vulkan device.",
            runtime.features().device_name.data(),
            runtime.features().device_type));
    }
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

/// Exact component breakdown for the current Phase-A unified-memory chaining
/// implementation. Keeping the components typed prevents a strict GPU decline
/// from collapsing into an unactionable total byte count.
struct VulkanChainingMemoryPlan {
    std::uint64_t anchor_count = 0;
    std::uint64_t device_anchor_count = 0;
    std::uint64_t range_count = 0;
    std::uint64_t device_anchor_bytes = 0;
    std::uint64_t device_score_bytes = 0;
    std::uint64_t device_predecessor_bytes = 0;
    std::uint64_t device_range_bytes = 0;
    std::uint64_t host_range_bytes = 0;
    std::uint64_t compacted_host_anchor_bytes = 0;
    std::uint64_t host_scored_index_bytes = 0;
    std::uint64_t host_visited_bytes = 0;
    std::uint64_t host_chain_index_bytes = 0;
    std::uint64_t coordinate_device_trace_bytes = 0;
    std::uint64_t coordinate_host_trace_bytes = 0;
    std::uint64_t gpu_phase_bytes = 0;
    std::uint64_t cpu_backtrack_phase_bytes = 0;
    std::uint64_t total_bytes = 0;
};

inline void recompute_vulkan_chaining_memory_total(
        VulkanChainingMemoryPlan& plan) noexcept {
    using bioseq::parallelism::saturating_add;
    const std::uint64_t common_host_bytes = saturating_add(
        plan.host_range_bytes, plan.compacted_host_anchor_bytes);
    plan.gpu_phase_bytes = common_host_bytes;
    for (const std::uint64_t component : {
            plan.device_anchor_bytes,
            plan.device_score_bytes,
            plan.device_predecessor_bytes,
            plan.device_range_bytes,
            plan.coordinate_device_trace_bytes,
            plan.coordinate_host_trace_bytes}) {
        plan.gpu_phase_bytes = saturating_add(
            plan.gpu_phase_bytes, component);
    }
    plan.cpu_backtrack_phase_bytes = common_host_bytes;
    for (const std::uint64_t component : {
            plan.device_score_bytes,
            plan.device_predecessor_bytes,
            plan.host_scored_index_bytes,
            plan.host_visited_bytes,
            plan.host_chain_index_bytes}) {
        plan.cpu_backtrack_phase_bytes = saturating_add(
            plan.cpu_backtrack_phase_bytes, component);
    }
    plan.total_bytes = std::max(
        plan.gpu_phase_bytes, plan.cpu_backtrack_phase_bytes);
}

[[nodiscard]] inline VulkanChainingMemoryPlan
make_vulkan_chaining_memory_plan(
        std::uint64_t anchor_count,
        std::uint64_t range_count,
        std::uint64_t device_anchor_count,
        bool compaction_required,
        bool coordinate_only) noexcept {
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    VulkanChainingMemoryPlan plan{
        .anchor_count = anchor_count,
        .device_anchor_count = device_anchor_count,
        .range_count = range_count,
        .device_anchor_bytes =
            saturating_mul(device_anchor_count, sizeof(Anchor)),
        .device_score_bytes =
            saturating_mul(device_anchor_count, sizeof(std::int32_t)),
        .device_predecessor_bytes =
            saturating_mul(device_anchor_count, sizeof(std::int64_t)),
        .device_range_bytes = saturating_mul(
            range_count, sizeof(bioseq::gpu::vulkan::GpuChainRange)),
        .host_range_bytes = saturating_mul(
            range_count, sizeof(bioseq::gpu::vulkan::GpuChainRange)),
        .compacted_host_anchor_bytes = compaction_required
            ? saturating_mul(device_anchor_count, sizeof(Anchor))
            : 0u,
    };
    if (coordinate_only) {
        plan.coordinate_device_trace_bytes = saturating_add(
            saturating_mul(device_anchor_count, 2u * sizeof(std::uint32_t)),
            sizeof(bioseq::gpu::vulkan::GpuCoordinateOnlyResultWire));
        plan.coordinate_host_trace_bytes =
            saturating_mul(device_anchor_count, sizeof(std::uint32_t));
    } else {
        plan.host_scored_index_bytes = saturating_mul(
            device_anchor_count, sizeof(ScoredAnchorIndex));
        plan.host_visited_bytes =
            saturating_mul(device_anchor_count, sizeof(std::int32_t));
        plan.host_chain_index_bytes =
            saturating_mul(device_anchor_count, sizeof(std::int64_t));
    }
    recompute_vulkan_chaining_memory_total(plan);
    return plan;
}

[[nodiscard]] inline VulkanChainingMemoryPlan
make_vulkan_chaining_memory_plan(
        std::uint64_t anchor_count,
        std::uint64_t range_count) noexcept {
    return make_vulkan_chaining_memory_plan(
        anchor_count, range_count, anchor_count,
        /*compaction_required=*/false,
        /*coordinate_only=*/false);
}

inline void apply_vulkan_chaining_allocation_requirements(
        VulkanChainingMemoryPlan& plan,
        bioseq::gpu::vulkan::Runtime& runtime,
        bool coordinate_only) {
    constexpr VkBufferUsageFlags input_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags output_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    plan.device_anchor_bytes = runtime.buffer_allocation_requirement(
        bioseq::parallelism::saturating_mul(
            plan.device_anchor_count, sizeof(Anchor)),
        input_usage);
    plan.device_range_bytes = runtime.buffer_allocation_requirement(
        bioseq::parallelism::saturating_mul(
            plan.range_count,
            sizeof(bioseq::gpu::vulkan::GpuChainRange)),
        input_usage);
    plan.device_score_bytes = runtime.buffer_allocation_requirement(
        bioseq::parallelism::saturating_mul(
            plan.device_anchor_count, sizeof(std::int32_t)),
        output_usage);
    plan.device_predecessor_bytes = runtime.buffer_allocation_requirement(
        bioseq::parallelism::saturating_mul(
            plan.device_anchor_count, sizeof(std::int64_t)),
        output_usage);
    if (coordinate_only) {
        plan.coordinate_device_trace_bytes = 0u;
        for (const std::uint64_t requested : {
                bioseq::parallelism::saturating_mul(
                    plan.device_anchor_count, sizeof(std::uint32_t)),
                bioseq::parallelism::saturating_mul(
                    plan.device_anchor_count, sizeof(std::uint32_t)),
                static_cast<std::uint64_t>(
                    sizeof(bioseq::gpu::vulkan::GpuCoordinateOnlyResultWire))}) {
            plan.coordinate_device_trace_bytes =
                bioseq::parallelism::saturating_add(
                    plan.coordinate_device_trace_bytes,
                    runtime.buffer_allocation_requirement(
                        requested, output_usage));
        }
    }
    recompute_vulkan_chaining_memory_total(plan);
}

class VulkanChainingAdmissionError final
    : public bioseq::gpu::vulkan::VulkanRuntimeError {
public:
    VulkanChainingMemoryPlan plan{};
    bioseq::parallelism::MemoryAdmissionFailure admission{};

    VulkanChainingAdmissionError(
            VulkanChainingMemoryPlan memory_plan,
            bioseq::parallelism::MemoryAdmissionFailure failure)
        : bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but memory admission failed "
            "(admission_reason={} required_bytes={} anchor_count={} "
            "device_anchor_count={} range_count={} "
            "device_anchor_bytes={} device_score_bytes={} "
            "device_predecessor_bytes={} device_range_bytes={} "
            "host_range_bytes={} compacted_host_anchor_bytes={} "
            "host_scored_index_bytes={} host_visited_bytes={} "
            "host_chain_index_bytes={} "
            "coordinate_device_trace_bytes={} "
            "coordinate_host_trace_bytes={} gpu_phase_bytes={} "
            "cpu_backtrack_phase_bytes={} "
            "allocatable_after_reservations_bytes={} reserved_bytes={} "
            "budget_ceiling_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={})",
            bioseq::parallelism::memory_admission_failure_reason_name(
                failure.reason),
            failure.requested_bytes,
            memory_plan.anchor_count,
            memory_plan.device_anchor_count,
            memory_plan.range_count,
            memory_plan.device_anchor_bytes,
            memory_plan.device_score_bytes,
            memory_plan.device_predecessor_bytes,
            memory_plan.device_range_bytes,
            memory_plan.host_range_bytes,
            memory_plan.compacted_host_anchor_bytes,
            memory_plan.host_scored_index_bytes,
            memory_plan.host_visited_bytes,
            memory_plan.host_chain_index_bytes,
            memory_plan.coordinate_device_trace_bytes,
            memory_plan.coordinate_host_trace_bytes,
            memory_plan.gpu_phase_bytes,
            memory_plan.cpu_backtrack_phase_bytes,
            failure.allocatable_after_reservations_bytes,
            failure.reserved_bytes,
            failure.budget_ceiling_bytes,
            failure.process_footprint_bytes,
            failure.system_available_bytes,
            failure.system_total_bytes,
            failure.process_soft_cap_bytes)),
          plan(memory_plan), admission(failure) {}
};

namespace detail {

/// Materialise the hard chaining ranges into a chain-arena array. The boundary
/// predicate matches `analyze_gpu_chaining_shape` exactly so the fill count equals
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

struct VulkanChainingShapePlan {
    std::size_t range_count = 0;
    std::size_t device_anchor_count = 0;
    bool compaction_required = false;
};

[[nodiscard]] inline VulkanChainingShapePlan analyze_gpu_chaining_shape(
        std::span<const Anchor> anchors,
        std::uint64_t max_ref_dist) noexcept {
    VulkanChainingShapePlan shape{};
    if (anchors.empty()) return shape;

    auto add_range = [&shape](std::size_t length) noexcept {
        ++shape.range_count;
        if (length > kGpuChainCompactRangeThreshold) {
            shape.compaction_required = true;
            const std::size_t stride =
                (length + kGpuChainCompactRangeTarget - 1u)
                / kGpuChainCompactRangeTarget;
            // Exact number selected by compact_long_gpu_chain_ranges:
            // 0,stride,... plus the last element when it is not already in
            // that progression.
            shape.device_anchor_count +=
                1u + ((length - 1u) + stride - 1u) / stride;
        } else {
            shape.device_anchor_count += length;
        }
    };

    std::size_t begin = 0u;
    for (std::size_t i = 1u; i < anchors.size(); ++i) {
        const Anchor& prev = anchors[i - 1u];
        const Anchor& cur = anchors[i];
        const bool key_changed =
            cur.ref_chromosome_key() != prev.ref_chromosome_key();
        const bool hard_ref_gap =
            cur.ref_position > prev.ref_position
            && cur.ref_position - prev.ref_position > max_ref_dist;
        if (key_changed || hard_ref_gap) {
            add_range(i - begin);
            begin = i;
        }
    }
    add_range(anchors.size() - begin);
    return shape;
}

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
            reserve_count +=
                1u + ((range.length - 1u) + stride - 1u) / stride;
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
[[nodiscard]] inline bool run_chain_anchors_vulkan_impl(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    const std::size_t n = anchors.size();
    const std::uint64_t n64 = static_cast<std::uint64_t>(n);
    if (n == 0) return false;
    if (!params.vulkan_enabled) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "Vulkan chaining selected but Minimap2Params disabled Vulkan");
    }
    if (n64 > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining selected but anchor count {} exceeds uint32_t", n64));
    }

    require_vulkan_chaining_supported();
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();

    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    if (max_dist_x < params.bandwidth) max_dist_x = params.bandwidth;
    if (max_dist_y < params.bandwidth) max_dist_y = params.bandwidth;

    const auto key = choose_vulkan_chaining_pipeline(runtime.features());

    const VulkanChainingShapePlan shape = analyze_gpu_chaining_shape(
        anchors, static_cast<std::uint64_t>(max_dist_x));
    const std::size_t range_count = shape.range_count;
    if (range_count == 0) return false;
    // DOC0353: the chaining kernel grid-strides one invocation per range and the
    // runtime clamps the dispatch to maxComputeWorkGroupCount[0], so range_count
    // is no longer bounded by the workgroup count — it is just the data size.

    VulkanChainingMemoryPlan memory_plan =
        make_vulkan_chaining_memory_plan(
            n64,
            range_count,
            static_cast<std::uint64_t>(shape.device_anchor_count),
            shape.compaction_required,
            params.coordinate_only);
    apply_vulkan_chaining_allocation_requirements(
        memory_plan, runtime, params.coordinate_only);
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(memory_plan.total_bytes, &admission_failure);
    if (!permit) {
        throw VulkanChainingAdmissionError{
            memory_plan, admission_failure};
    }

    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto ranges = build_gpu_chain_ranges(
        anchors,
        static_cast<std::uint64_t>(max_dist_x),
        range_count);

    std::span<const Anchor> dp_anchors = anchors;
    std::span<const bioseq::gpu::vulkan::GpuChainRange> dp_ranges = ranges;
    ArenaSlab<Anchor> compacted_anchors = make_arena_slab<Anchor>();
    if (compact_long_gpu_chain_ranges(
            anchors, ranges, compacted_anchors, ranges)) {
        dp_anchors = compacted_anchors.as_const_span();
        dp_ranges = ranges;
    }

    const std::size_t dp_n = dp_anchors.size();
    const std::uint64_t dp_n64 = static_cast<std::uint64_t>(dp_n);
    if (dp_n != shape.device_anchor_count) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Vulkan chaining shape preflight mismatch: planned {} anchors, "
            "materialized {}",
            shape.device_anchor_count,
            dp_n));
    }

    // Unified-memory Phase A writes/reads the mapped device buffers directly;
    // the legacy transfer parameter remains an invalid sentinel and allocates
    // no 24*N bounce buffer.
    bioseq::gpu::vulkan::Buffer transfer_scratch{};
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
    if (anchors_buf.mapped == nullptr || ranges_buf.mapped == nullptr
        || score_buf.mapped == nullptr || pred_buf.mapped == nullptr
        || !anchors_buf.host_visible_coherent()
        || !ranges_buf.host_visible_coherent()
        || !score_buf.host_visible_coherent()
        || !pred_buf.host_visible_coherent()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "Vulkan chaining requires host-visible coherent unified-memory buffers");
    }
    std::span<int32_t> best_scores{
        static_cast<int32_t*>(score_buf.mapped), dp_n};
    std::span<int64_t> predecessors{
        static_cast<int64_t*>(pred_buf.mapped), dp_n};
    for (std::size_t i = 0; i < dp_n; ++i) {
        best_scores[i] = static_cast<int32_t>(dp_anchors[i].query_span);
        predecessors[i] = -1;
    }
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
            std::as_bytes(std::span<const int32_t>{best_scores}),
            std::as_bytes(std::span<const int64_t>{predecessors}),
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
            return true;
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
        return true;
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
        std::as_writable_bytes(best_scores),
        std::as_writable_bytes(predecessors),
        static_cast<std::uint32_t>(dp_n64),
        static_cast<std::uint32_t>(dp_ranges.size()),
        max_dist_x,
        max_dist_y,
        params.bandwidth,
        gpu_max_chain_iter,
        params.gap_penalty,
        params.skip_penalty,
        params.max_chain_skip);

    // The queue fence completed inside the runtime call.  Anchors and hard
    // ranges are host-owned below and their device copies are dead before CPU
    // validation/backtracking allocates its 28*N arena scratch.  This lifetime
    // split is what makes the admitted peak `max(gpu_phase, cpu_phase)` rather
    // than the sum of both phases.
    anchors_buf.reset();
    ranges_buf.reset();

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
        std::span<const int32_t>{best_scores},
        std::span<const int64_t>{predecessors},
        std::span<const int32_t>{},
        params.min_chain_score,
        params.min_seed_count,
        chains_out,
        reordered_anchors_out);
    return true;
}

}  // namespace detail

/// Public entry point. Pins the `Anchor` ABI and runs the Vulkan tiled-DP path.
/// If this backend is selected, no scalar fallback is allowed here.
inline bool chain_anchors_vulkan(
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

    return detail::run_chain_anchors_vulkan_impl(
        anchors, params, chains_out, reordered_anchors_out);
}

}  // namespace bioseq::chaining
