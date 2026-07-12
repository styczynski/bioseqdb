/**
 * @file phase_b_abi.cppm
 * @brief Vulkan primitive ABI structs shared by the runtime, Phase-C
 *        minimizer-sketch/GPU-index, and seeding modules.
 *
 * This module deliberately imports NOTHING from `bioseq_gpu.vulkan.runtime` or
 * the minimizer/index/seeding modules, so it can sit at the bottom of the Phase
 * B/C dependency graph. It owns the host-side wire records (`SortRangeWire`,
 * `VulkanSketchParams`, `VulkanRankLut`) whose byte layout must stay identical
 * to the GLSL structs the remaining shaders declare, plus the small enums that
 * select the shader specialisation key (alphabet kind / minimizer word width).
 * Every layout is pinned with `static_assert` against the field offsets the
 * SPIR-V reflection asserts at pipeline-registration time (DOC0307 §4.3).
 */
module;

#include <cstddef>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

export module bioseq.gpu.vulkan.phase_b_abi;

export namespace bioseq::gpu::vulkan {

/// Which lowest-level shader specialisation reads the packed sequence. Only the
/// shader body specialises on this; every C++ function above the shader call
/// stays alphabet-generic (DOC0307 §0, §9).
enum class AlphabetKernelKind : std::uint8_t {
    dna4_nt4 = 0,
    dna5_nt4 = 1,
    generic = 2,
};

/// Minimizer storage width selector. Mirrors `MinimizerRepr<L,K>::storage_t`
/// (16/32/64-bit single integer) plus the 128-bit `u64x2` fallback.
enum class MinimizerWordKind : std::uint8_t {
    u16 = 0,
    u32 = 1,
    u64 = 2,
    u64x2 = 3,
};

/// Rank → nt4 code lookup uploaded to minimizer/seed shaders. `rank_to_code`
/// maps an alphabet rank to its forward nt4 code (>=4 == ambiguity/reset);
/// `rank_to_rc_code` is the reverse-complement code (DOC0307 §6.1).
struct VulkanRankLut {
    std::uint32_t rank_to_code[256]{};
    std::uint32_t rank_to_rc_code[256]{};
};
static_assert(sizeof(VulkanRankLut) == 2048);

/// Push-constant/uniform record carrying the runtime sketch parameters. The
/// Phase-C sketch/index shaders read distinct subsets of this block; the C++
/// side fills it once from `make_sequence_upload_view` + `minimizer_wire_traits`.
struct VulkanSketchParams {
    std::uint32_t bp_size = 0;
    std::uint32_t window_size = 10;
    std::uint32_t kmer_size = 15;
    std::uint32_t hash_bits = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32 = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t seq_word_offset = 0;
    std::uint32_t char_mask = 0;
    std::uint32_t padding_letters = 0;
};
static_assert(sizeof(VulkanSketchParams) == 64);

/// One sortable anchor segment `(begin, length)` for the bitonic anchor sort.
struct SortRangeWire {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};
static_assert(sizeof(SortRangeWire) == 8);
static_assert(offsetof(SortRangeWire, begin) == 0);
static_assert(offsetof(SortRangeWire, length) == 4);

enum class SeedAnchorSortBackend : std::uint8_t {
    none,
    bitonic,
    radix,
};

enum class SeedAnchorSortPlanFailure : std::uint8_t {
    none,
    anchor_count_exceeds_u32,
    target_coordinate_invalid,
    dispatch_limit_unavailable,
    radix_workgroup_geometry_unsupported,
    storage_buffer_range_exceeded,
    arithmetic_overflow,
};

/// Allocation/dispatch contract for one Phase-B GPU anchor sort. The plan is
/// pure and deterministic so admission, the runtime, and boundary tests use the
/// same arithmetic. `device_sort_bytes` includes the primary anchor/key arrays,
/// all radix ping-pong scratch, and the histogram (or the 8-byte bitonic range);
/// `host_result_bytes` is the one downloaded 24-byte anchor array.
struct SeedAnchorSortPlan {
    SeedAnchorSortBackend backend = SeedAnchorSortBackend::none;
    SeedAnchorSortPlanFailure failure = SeedAnchorSortPlanFailure::none;
    std::uint64_t anchor_count = 0;
    std::uint64_t anchor_bytes = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t histogram_bytes = 0;
    std::uint64_t device_sort_bytes = 0;
    std::uint64_t host_result_bytes = 0;
    std::uint64_t transient_bytes = 0;
    // Legacy Phase-B anchors and keys remain segment-local. The offset is
    // carried in the plan so admission/runtime validation cannot accidentally
    // validate a different target segment. `max_absolute_ref_position` proves
    // adding this one constant to every local coordinate is overflow-free and
    // therefore order-preserving, without changing the legacy Anchor ABI.
    std::uint64_t target_position_offset = 0;
    std::uint64_t max_local_ref_position = 0;
    std::uint64_t max_absolute_ref_position = 0;
    std::uint32_t target_ref_bits = 0;
    std::uint32_t radix_pass_count = 0;
    std::uint32_t num_workgroups = 0;
    std::uint32_t elements_per_workgroup = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return failure == SeedAnchorSortPlanFailure::none;
    }

    [[nodiscard]] constexpr bool operator==(
        const SeedAnchorSortPlan&) const noexcept = default;
};

[[nodiscard]] inline constexpr bool seed_radix_result_in_scratch(
        std::uint32_t pass_count) noexcept {
    return (pass_count & 1u) != 0u;
}

[[nodiscard]] inline constexpr std::uint32_t
seed_anchor_bitonic_capacity(
        std::uint32_t max_workgroup_invocations,
        std::uint32_t max_workgroup_size_x,
        std::uint32_t max_shared_memory_bytes) noexcept {
    constexpr std::uint32_t max_shader_capacity = 1024u;
    constexpr std::uint32_t per_lane_shared_bytes = 24u + 4u;
    std::uint32_t capacity = 0u;
    for (std::uint32_t candidate = 1u;
         candidate <= max_shader_capacity;
         candidate <<= 1u) {
        if (candidate > max_workgroup_invocations
            || candidate > max_workgroup_size_x
            || static_cast<std::uint64_t>(candidate)
                    * per_lane_shared_bytes
                > max_shared_memory_bytes) {
            break;
        }
        capacity = candidate;
    }
    return capacity;
}

[[nodiscard]] inline constexpr std::uint64_t seed_sort_sat_add(
        std::uint64_t lhs,
        std::uint64_t rhs) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    return lhs > max - rhs ? max : lhs + rhs;
}

[[nodiscard]] inline constexpr std::uint64_t seed_sort_sat_mul(
        std::uint64_t lhs,
        std::uint64_t rhs) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    return lhs != 0u && rhs > max / lhs ? max : lhs * rhs;
}

[[nodiscard]] inline constexpr SeedAnchorSortPlan make_seed_anchor_sort_plan(
        std::uint64_t anchor_count,
        std::uint64_t target_length,
        std::uint64_t target_position_offset,
        std::uint32_t bitonic_capacity,
        std::uint32_t max_workgroup_count_x,
        std::uint32_t max_workgroup_invocations,
        std::uint32_t max_workgroup_size_x,
        std::uint64_t max_storage_buffer_range) noexcept {
    SeedAnchorSortPlan plan{
        .anchor_count = anchor_count,
        .target_position_offset = target_position_offset};
    if (anchor_count == 0u) return plan;
    if (anchor_count > (std::numeric_limits<std::uint32_t>::max)()) {
        plan.failure = SeedAnchorSortPlanFailure::anchor_count_exceeds_u32;
        return plan;
    }
    if (target_length == 0u) {
        plan.failure = SeedAnchorSortPlanFailure::target_coordinate_invalid;
        return plan;
    }
    const std::uint64_t last_local = target_length - 1u;
    constexpr std::uint64_t first_invalid_coordinate = std::uint64_t{1} << 63u;
    if (last_local
            > (std::numeric_limits<std::uint64_t>::max)()
                - target_position_offset
        || target_position_offset + last_local >= first_invalid_coordinate) {
        plan.failure = SeedAnchorSortPlanFailure::target_coordinate_invalid;
        return plan;
    }
    const std::uint64_t max_coordinate = target_position_offset + last_local;
    plan.max_local_ref_position = last_local;
    plan.max_absolute_ref_position = max_coordinate;
    plan.target_ref_bits = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::bit_width(last_local)));
    plan.radix_pass_count = std::clamp<std::uint32_t>(
        (plan.target_ref_bits + 8u) / 8u, 1u, 8u);
    plan.anchor_bytes = seed_sort_sat_mul(anchor_count, 24u);
    plan.key_bytes = seed_sort_sat_mul(anchor_count, 8u);
    plan.host_result_bytes = plan.anchor_bytes;
    if (plan.anchor_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || plan.key_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        plan.failure = SeedAnchorSortPlanFailure::arithmetic_overflow;
        return plan;
    }

    if (bitonic_capacity != 0u && anchor_count <= bitonic_capacity) {
        plan.backend = SeedAnchorSortBackend::bitonic;
        plan.device_sort_bytes = seed_sort_sat_add(
            seed_sort_sat_add(plan.anchor_bytes, plan.key_bytes),
            sizeof(SortRangeWire));
        plan.transient_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.host_result_bytes);
    } else {
        if (max_workgroup_count_x == 0u) {
            plan.failure = SeedAnchorSortPlanFailure::dispatch_limit_unavailable;
            return plan;
        }
        constexpr std::uint32_t radix_local_size = 256u;
        if (max_workgroup_invocations < radix_local_size
            || max_workgroup_size_x < radix_local_size) {
            plan.failure = SeedAnchorSortPlanFailure::
                radix_workgroup_geometry_unsupported;
            return plan;
        }
        plan.backend = SeedAnchorSortBackend::radix;
        constexpr std::uint64_t default_elements_per_workgroup = 512u;
        std::uint64_t workgroups =
            (anchor_count + default_elements_per_workgroup - 1u)
                / default_elements_per_workgroup;
        if (workgroups > max_workgroup_count_x) {
            workgroups = max_workgroup_count_x;
        }
        plan.num_workgroups = static_cast<std::uint32_t>(workgroups);
        plan.elements_per_workgroup = static_cast<std::uint32_t>(
            (anchor_count + workgroups - 1u) / workgroups);
        plan.histogram_bytes = seed_sort_sat_mul(
            seed_sort_sat_mul(workgroups, 256u), sizeof(std::uint32_t));
        plan.device_sort_bytes = 0u;
        plan.device_sort_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.anchor_bytes);
        plan.device_sort_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.key_bytes);
        plan.device_sort_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.anchor_bytes);
        plan.device_sort_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.key_bytes);
        plan.device_sort_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.histogram_bytes);
        plan.transient_bytes = seed_sort_sat_add(
            plan.device_sort_bytes, plan.host_result_bytes);
    }

    if (plan.device_sort_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || plan.transient_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        plan.failure = SeedAnchorSortPlanFailure::arithmetic_overflow;
        return plan;
    }
    if (max_storage_buffer_range == 0u
        || plan.anchor_bytes > max_storage_buffer_range
        || plan.key_bytes > max_storage_buffer_range
        || plan.histogram_bytes > max_storage_buffer_range
        || (plan.backend == SeedAnchorSortBackend::bitonic
            && sizeof(SortRangeWire) > max_storage_buffer_range)) {
        plan.failure = SeedAnchorSortPlanFailure::storage_buffer_range_exceeded;
    }
    return plan;
}

/// Operation-originated proof returned only after the selected GPU seed sort
/// and the anchor download complete. No selector flag can manufacture it.
struct SeedLookupSortExecutionProof {
    std::uint64_t bitonic_sort_dispatches = 0;
    std::uint64_t radix_sort_operations = 0;
    std::uint64_t radix_anchor_count = 0;
    std::uint64_t radix_passes = 0;
    std::uint64_t radix_histogram_dispatches = 0;
    std::uint64_t radix_scan_dispatches = 0;
    std::uint64_t radix_scatter_dispatches = 0;
    std::uint64_t host_sort_fallbacks = 0;

    [[nodiscard]] constexpr bool operator==(
        const SeedLookupSortExecutionProof&) const noexcept = default;
};

/// Result of a seed-lookup roundtrip: the downloaded `Anchor` bytes plus the
/// real anchor count. The caller memcpys `count * sizeof(Anchor)` into the
/// pipeline `ArenaSlab<Anchor>`.
struct SeedLookupResult {
    std::vector<std::byte> host_bytes;
    std::uint64_t count = 0;
    std::uint64_t seed_lookup_dispatches = 0;
    std::uint64_t open_addressed_lookup_dispatches = 0;
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    SeedLookupSortExecutionProof sort_proof{};
};

} // namespace bioseq::gpu::vulkan
