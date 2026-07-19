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
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
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

/// Host mirror of the explicit 24-byte Anchor payload emitted by the seed
/// shader.  This deliberately names every field instead of hashing raw object
/// bytes, so padding can never become part of the biological-result proof.
struct SeedAnchorWire {
    std::uint64_t ref_position = 0u;
    std::uint64_t query_position = 0u;
    std::uint32_t is_reverse = 0u;
    std::uint8_t query_span = 0u;
    std::uint8_t flags = 0u;
    std::uint16_t segment_id = 0u;
};
static_assert(sizeof(SeedAnchorWire) == 24u);
static_assert(offsetof(SeedAnchorWire, ref_position) == 0u);
static_assert(offsetof(SeedAnchorWire, query_position) == 8u);
static_assert(offsetof(SeedAnchorWire, is_reverse) == 16u);
static_assert(offsetof(SeedAnchorWire, query_span) == 20u);
static_assert(offsetof(SeedAnchorWire, flags) == 21u);
static_assert(offsetof(SeedAnchorWire, segment_id) == 22u);

inline constexpr std::uint64_t kSeedAnchorDigestSeedLo =
    0x243f6a8885a308d3ULL;
inline constexpr std::uint64_t kSeedAnchorDigestSeedHi =
    0x13198a2e03707344ULL;

struct SeedAnchorCanonicalProof {
    std::uint64_t anchor_count = 0u;
    std::uint64_t digest_lo = kSeedAnchorDigestSeedLo;
    std::uint64_t digest_hi = kSeedAnchorDigestSeedHi;
    bool canonical_order_checked = false;
    bool canonical_order_accepted = false;
};

struct SeedAnchorTieNormalizationProof {
    std::uint64_t canonical_tie_run_count = 0u;
    std::uint64_t canonical_tie_anchor_count = 0u;
    bool gpu_ref_order_checked = false;
    bool gpu_ref_order_accepted = false;
};

[[nodiscard]] inline constexpr std::uint64_t
seed_anchor_digest_mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31u);
}

[[nodiscard]] inline constexpr bool seed_anchor_wire_less(
    const SeedAnchorWire& left,
    const SeedAnchorWire& right) noexcept
{
    constexpr std::uint64_t position_mask =
        (std::uint64_t{1} << 63u) - 1u;
    const std::uint64_t left_ref =
        (static_cast<std::uint64_t>(left.is_reverse) << 63u)
        | (left.ref_position & position_mask);
    const std::uint64_t right_ref =
        (static_cast<std::uint64_t>(right.is_reverse) << 63u)
        | (right.ref_position & position_mask);
    if (left_ref != right_ref) return left_ref < right_ref;

    const std::uint64_t left_query =
        (static_cast<std::uint64_t>(left.segment_id) << 48u)
        | (static_cast<std::uint64_t>(left.flags) << 40u)
        | (static_cast<std::uint64_t>(left.query_span) << 32u)
        | static_cast<std::uint32_t>(left.query_position);
    const std::uint64_t right_query =
        (static_cast<std::uint64_t>(right.segment_id) << 48u)
        | (static_cast<std::uint64_t>(right.flags) << 40u)
        | (static_cast<std::uint64_t>(right.query_span) << 32u)
        | static_cast<std::uint32_t>(right.query_position);
    if (left_query != right_query) return left_query < right_query;
    return left.query_position < right.query_position;
}

[[nodiscard]] inline SeedAnchorWire seed_anchor_wire_at(
    std::span<const std::byte> payload,
    std::uint64_t index) noexcept
{
    SeedAnchorWire anchor{};
    std::memcpy(
        &anchor,
        payload.data() + index * sizeof(SeedAnchorWire),
        sizeof(anchor));
    return anchor;
}

inline void seed_anchor_wire_store(
    std::span<std::byte> payload,
    std::uint64_t index,
    const SeedAnchorWire& anchor) noexcept
{
    std::memcpy(
        payload.data() + index * sizeof(SeedAnchorWire),
        &anchor,
        sizeof(anchor));
}

[[nodiscard]] inline constexpr std::uint64_t seed_anchor_ref_bits(
    const SeedAnchorWire& anchor) noexcept
{
    constexpr std::uint64_t position_mask =
        (std::uint64_t{1} << 63u) - 1u;
    return (static_cast<std::uint64_t>(anchor.is_reverse) << 63u)
        | (anchor.ref_position & position_mask);
}

inline void seed_anchor_wire_swap(
    std::span<std::byte> payload,
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    if (left == right) return;
    const SeedAnchorWire left_anchor = seed_anchor_wire_at(payload, left);
    const SeedAnchorWire right_anchor = seed_anchor_wire_at(payload, right);
    seed_anchor_wire_store(payload, left, right_anchor);
    seed_anchor_wire_store(payload, right, left_anchor);
}

/// Sort one equal-ref run by the remaining CanonicalAnchorLess fields.  This
/// allocation-free heap sort is part of the required common-order
/// normalization after the GPU's ref_bits-only sort; it is not a biological
/// CPU fallback.
inline void normalize_seed_anchor_tie_run(
    std::span<std::byte> payload,
    std::uint64_t begin,
    std::uint64_t count) noexcept
{
    const auto sift_down = [payload, begin](
            std::uint64_t heap_count,
            std::uint64_t first_root) noexcept {
        std::uint64_t root = first_root;
        while (root < heap_count / 2u) {
            std::uint64_t child = root * 2u + 1u;
            if (child + 1u < heap_count
                && seed_anchor_wire_less(
                    seed_anchor_wire_at(payload, begin + child),
                    seed_anchor_wire_at(payload, begin + child + 1u))) {
                ++child;
            }
            if (!seed_anchor_wire_less(
                    seed_anchor_wire_at(payload, begin + root),
                    seed_anchor_wire_at(payload, begin + child))) {
                return;
            }
            seed_anchor_wire_swap(payload, begin + root, begin + child);
            root = child;
        }
    };
    for (std::uint64_t root = count / 2u; root != 0u; --root) {
        sift_down(count, root - 1u);
    }
    for (std::uint64_t end = count; end > 1u; --end) {
        seed_anchor_wire_swap(payload, begin, begin + end - 1u);
        sift_down(end - 1u, 0u);
    }
}

[[nodiscard]] inline SeedAnchorTieNormalizationProof
normalize_seed_anchor_equal_ref_ties(
    std::span<std::byte> payload,
    std::uint64_t anchor_count) noexcept
{
    SeedAnchorTieNormalizationProof proof{
        .gpu_ref_order_checked = true,
        .gpu_ref_order_accepted = true,
    };
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (anchor_count > max / sizeof(SeedAnchorWire)
        || payload.size() != anchor_count * sizeof(SeedAnchorWire)) {
        proof.gpu_ref_order_accepted = false;
        return proof;
    }
    std::uint64_t run_begin = 0u;
    while (run_begin < anchor_count) {
        const SeedAnchorWire first = seed_anchor_wire_at(payload, run_begin);
        if (first.is_reverse > 1u) {
            proof.gpu_ref_order_accepted = false;
            return proof;
        }
        const std::uint64_t run_ref = seed_anchor_ref_bits(first);
        if (run_begin != 0u) {
            const SeedAnchorWire previous =
                seed_anchor_wire_at(payload, run_begin - 1u);
            if (seed_anchor_ref_bits(previous) > run_ref) {
                proof.gpu_ref_order_accepted = false;
                return proof;
            }
        }
        std::uint64_t run_end = run_begin + 1u;
        while (run_end < anchor_count) {
            const SeedAnchorWire next = seed_anchor_wire_at(payload, run_end);
            if (next.is_reverse > 1u) {
                proof.gpu_ref_order_accepted = false;
                return proof;
            }
            const std::uint64_t next_ref = seed_anchor_ref_bits(next);
            if (next_ref < run_ref) {
                proof.gpu_ref_order_accepted = false;
                return proof;
            }
            if (next_ref != run_ref) break;
            ++run_end;
        }
        const std::uint64_t run_count = run_end - run_begin;
        if (run_count > 1u) {
            ++proof.canonical_tie_run_count;
            proof.canonical_tie_anchor_count += run_count;
            normalize_seed_anchor_tie_run(payload, run_begin, run_count);
        }
        run_begin = run_end;
    }
    return proof;
}

/// Validate CanonicalAnchorLess-equivalent order and digest every explicit
/// Anchor field in order.  The two lanes have independent seeds, salts, and
/// recurrence, and the empty payload therefore has deterministic nonzero
/// digest seeds while still carrying checked/accepted evidence.
[[nodiscard]] inline SeedAnchorCanonicalProof make_seed_anchor_canonical_proof(
    std::span<const std::byte> payload,
    std::uint64_t anchor_count) noexcept
{
    SeedAnchorCanonicalProof proof{
        .anchor_count = anchor_count,
        .canonical_order_checked = true,
        .canonical_order_accepted = true,
    };
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (anchor_count > max / sizeof(SeedAnchorWire)
        || payload.size() != anchor_count * sizeof(SeedAnchorWire)) {
        proof.canonical_order_accepted = false;
        return proof;
    }

    SeedAnchorWire previous{};
    bool have_previous = false;
    for (std::uint64_t index = 0u; index < anchor_count; ++index) {
        SeedAnchorWire anchor{};
        std::memcpy(
            &anchor,
            payload.data() + index * sizeof(SeedAnchorWire),
            sizeof(SeedAnchorWire));
        if (anchor.is_reverse > 1u
            || (have_previous && seed_anchor_wire_less(anchor, previous))) {
            proof.canonical_order_accepted = false;
        }

        const std::array<std::uint64_t, 6u> fields{
            anchor.ref_position,
            anchor.query_position,
            anchor.is_reverse,
            anchor.query_span,
            anchor.flags,
            anchor.segment_id,
        };
        for (std::size_t field_index = 0u;
             field_index < fields.size(); ++field_index) {
            const std::uint64_t tagged = seed_anchor_digest_mix(
                fields[field_index]
                ^ seed_anchor_digest_mix(
                    (index << 3u) | field_index));
            proof.digest_lo = seed_anchor_digest_mix(
                proof.digest_lo ^ tagged ^ 0xa4093822299f31d0ULL);
            proof.digest_hi = std::rotl(
                proof.digest_hi + tagged + 0x082efa98ec4e6c89ULL,
                23);
            proof.digest_hi ^= seed_anchor_digest_mix(
                tagged ^ 0x452821e638d01377ULL);
        }
        previous = anchor;
        have_previous = true;
    }
    return proof;
}

/// Result of a seed-lookup roundtrip: the downloaded `Anchor` bytes plus the
/// real anchor count. The caller memcpys `count * sizeof(Anchor)` into the
/// pipeline `ArenaSlab<Anchor>`.
struct SeedLookupResult {
    std::vector<std::byte> host_bytes;
    std::uint64_t count = 0;
    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    SeedAnchorCanonicalProof anchor_proof{};
    std::uint64_t canonical_tie_normalization_runs = 0u;
    std::uint64_t canonical_tie_normalization_anchors = 0u;
    std::uint64_t seed_lookup_dispatches = 0;
    std::uint64_t open_addressed_lookup_dispatches = 0;
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    SeedLookupSortExecutionProof sort_proof{};
};

} // namespace bioseq::gpu::vulkan
