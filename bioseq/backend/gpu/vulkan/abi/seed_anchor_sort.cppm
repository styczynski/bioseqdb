/**
 * @file
 * @brief Plans GPU anchor sorting and checks the resulting chaining order.
 */
module;

#include <cstddef>

export module bioseq.gpu.vulkan.abi:seed_anchor_sort;

import std;
import bioseq.common.checked_arithmetic;

export namespace bioseq::gpu::vulkan {

/** @brief Identifies one sortable anchor segment as `(begin, length)`. */
struct sort_range_wire {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};
static_assert(sizeof(sort_range_wire) == 8);
static_assert(offsetof(sort_range_wire, begin) == 0);
static_assert(offsetof(sort_range_wire, length) == 4);

/** @brief Identifies the selected device anchor-sort implementation. */
enum class seed_anchor_sort_backend : std::uint8_t {
    none,
    bitonic,
    radix,
};

/** @brief Reason an anchor-sort request cannot run. */
enum class seed_anchor_sort_plan_failure : std::uint8_t {
    none,
    anchor_count_exceeds_u32,
    target_coordinate_invalid,
    dispatch_limit_unavailable,
    radix_workgroup_geometry_unsupported,
    storage_buffer_range_exceeded,
    arithmetic_overflow,
};

/**
 * @brief Buffer sizes and dispatch settings for one anchor sort.
 *
 * `device_sort_bytes` includes anchors, keys, and scratch buffers for the
 * selected sort. `host_result_bytes` covers the downloaded 24-byte anchor
 * records.
 */
struct seed_anchor_sort_plan {
    seed_anchor_sort_backend backend = seed_anchor_sort_backend::none;
    seed_anchor_sort_plan_failure failure = seed_anchor_sort_plan_failure::none;
    std::uint64_t anchor_count = 0;
    std::uint64_t anchor_bytes = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t histogram_bytes = 0;
    std::uint64_t device_sort_bytes = 0;
    std::uint64_t host_result_bytes = 0;
    std::uint64_t transient_bytes = 0;
    // Anchors and sort keys use coordinates within the target segment. Record
    // its offset  and largest position so the runtime can check that adding
    // the offset preserves order and cannot overflow.
    std::uint64_t target_position_offset = 0;
    std::uint64_t max_local_ref_position = 0;
    std::uint64_t max_absolute_ref_position = 0;
    std::uint32_t target_ref_bits = 0;
    std::uint32_t radix_pass_count = 0;
    std::uint32_t num_workgroups = 0;
    std::uint32_t elements_per_workgroup = 0;

    /** @brief Returns true if planning found no edror, including an empty sort */
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return failure == seed_anchor_sort_plan_failure::none;
    }

    /** @brief Compares all buffer sizes, limits, and dispatch settings. */
    [[nodiscard]] constexpr bool operator==(const seed_anchor_sort_plan&) const noexcept = default;
};

/**
 * @brief Returns whether the last radix pass writes into the scratch buffer.
 *
 * Odd pass counts leave the result in scratch. Even counts leave it in the
 * original buffer.
 */
[[nodiscard]] inline constexpr bool
is_seed_radix_result_in_scratch(std::uint32_t pass_count) noexcept {
    return (pass_count & 1u) != 0u;
}

/**
 * @brief Finds the largest bitonic-sort capacity supported by the device.
 *
 * The result is a power of two that fits the invocation and shared-memory
 * limits, up to the shader maximum. Zero means even  one lane will not fit.
 */
[[nodiscard]] inline constexpr std::uint32_t
compute_seed_anchor_bitonic_capacity(std::uint32_t max_workgroup_invocations,
                                     std::uint32_t max_workgroup_size_x,
                                     std::uint32_t max_shared_memory_bytes) noexcept {
    constexpr std::uint32_t max_shader_capacity = 1024u;
    constexpr std::uint32_t per_lane_shared_bytes = 24u + 4u;
    std::uint32_t capacity = 0u;
    for (std::uint32_t candidate = 1u; candidate <= max_shader_capacity; candidate <<= 1u) {
        if (candidate > max_workgroup_invocations || candidate > max_workgroup_size_x ||
            static_cast<std::uint64_t>(candidate) * per_lane_shared_bytes >
                max_shared_memory_bytes) {
            break;
        }
        capacity = candidate;
    }
    return capacity;
}

/**
 * @brief Chooses an anchor sorter and calculates its buffer and dispatch sizes.
 *
 * Small inputs use one bitonic workgroup. Larger inputs use radix sorting,
 * with enough passes for the segment-local coordinate and strand. Target
 * coordinates must remain valid when the segment offset is added.
 *
 * The returned failure explains unsupported device limits or arithmetic
 * overflow. Zero anchors produce a valid plan with no sorting work.
 */
[[nodiscard]] inline constexpr seed_anchor_sort_plan
make_seed_anchor_sort_plan(std::uint64_t anchor_count,
                           std::uint64_t target_length,
                           std::uint64_t target_position_offset,
                           std::uint32_t bitonic_capacity,
                           std::uint32_t max_workgroup_count_x,
                           std::uint32_t max_workgroup_invocations,
                           std::uint32_t max_workgroup_size_x,
                           std::uint64_t max_storage_buffer_range) noexcept {
    seed_anchor_sort_plan plan{.anchor_count = anchor_count,
                               .target_position_offset = target_position_offset};
    if (anchor_count == 0u)
        return plan;
    if (anchor_count > (std::numeric_limits<std::uint32_t>::max)()) {
        plan.failure = seed_anchor_sort_plan_failure::anchor_count_exceeds_u32;
        return plan;
    }
    if (target_length == 0u) {
        plan.failure = seed_anchor_sort_plan_failure::target_coordinate_invalid;
        return plan;
    }
    const std::uint64_t last_local = target_length - 1u;
    constexpr std::uint64_t first_invalid_coordinate = std::uint64_t{1} << 63u;
    if (last_local > (std::numeric_limits<std::uint64_t>::max)() - target_position_offset ||
        target_position_offset + last_local >= first_invalid_coordinate) {
        plan.failure = seed_anchor_sort_plan_failure::target_coordinate_invalid;
        return plan;
    }
    const std::uint64_t max_coordinate = target_position_offset + last_local;
    plan.max_local_ref_position = last_local;
    plan.max_absolute_ref_position = max_coordinate;
    plan.target_ref_bits =
        std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::bit_width(last_local)));
    plan.radix_pass_count = std::clamp<std::uint32_t>((plan.target_ref_bits + 8u) / 8u, 1u, 8u);
    plan.anchor_bytes = bioseq::common::saturating_mul(anchor_count, 24u);
    plan.key_bytes = bioseq::common::saturating_mul(anchor_count, 8u);
    plan.host_result_bytes = plan.anchor_bytes;
    if (plan.anchor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        plan.key_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        plan.failure = seed_anchor_sort_plan_failure::arithmetic_overflow;
        return plan;
    }

    if (bitonic_capacity != 0u && anchor_count <= bitonic_capacity) {
        plan.backend = seed_anchor_sort_backend::bitonic;
        plan.device_sort_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_add(plan.anchor_bytes, plan.key_bytes),
            sizeof(sort_range_wire));
        plan.transient_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.host_result_bytes);
    } else {
        if (max_workgroup_count_x == 0u) {
            plan.failure = seed_anchor_sort_plan_failure::dispatch_limit_unavailable;
            return plan;
        }
        constexpr std::uint32_t radix_local_size = 256u;
        if (max_workgroup_invocations < radix_local_size ||
            max_workgroup_size_x < radix_local_size) {
            plan.failure = seed_anchor_sort_plan_failure::radix_workgroup_geometry_unsupported;
            return plan;
        }
        plan.backend = seed_anchor_sort_backend::radix;
        constexpr std::uint64_t default_elements_per_workgroup = 512u;
        std::uint64_t workgroups =
            (anchor_count + default_elements_per_workgroup - 1u) / default_elements_per_workgroup;
        if (workgroups > max_workgroup_count_x) {
            workgroups = max_workgroup_count_x;
        }
        plan.num_workgroups = static_cast<std::uint32_t>(workgroups);
        plan.elements_per_workgroup =
            static_cast<std::uint32_t>((anchor_count + workgroups - 1u) / workgroups);
        plan.histogram_bytes = bioseq::common::saturating_mul(
            bioseq::common::saturating_mul(workgroups, 256u), sizeof(std::uint32_t));
        plan.device_sort_bytes = 0u;
        plan.device_sort_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.anchor_bytes);
        plan.device_sort_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.key_bytes);
        plan.device_sort_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.anchor_bytes);
        plan.device_sort_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.key_bytes);
        plan.device_sort_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.histogram_bytes);
        plan.transient_bytes =
            bioseq::common::saturating_add(plan.device_sort_bytes, plan.host_result_bytes);
    }

    if (plan.device_sort_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        plan.transient_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        plan.failure = seed_anchor_sort_plan_failure::arithmetic_overflow;
        return plan;
    }
    if (max_storage_buffer_range == 0u || plan.anchor_bytes > max_storage_buffer_range ||
        plan.key_bytes > max_storage_buffer_range ||
        plan.histogram_bytes > max_storage_buffer_range ||
        (plan.backend == seed_anchor_sort_backend::bitonic &&
         sizeof(sort_range_wire) > max_storage_buffer_range)) {
        plan.failure = seed_anchor_sort_plan_failure::storage_buffer_range_exceeded;
    }
    return plan;
}

/** @brief Records completed device sort operations and host fallback counts. */
struct seed_lookup_sort_execution_stats {
    std::uint64_t bitonic_sort_dispatches = 0;
    std::uint64_t radix_sort_operations = 0;
    std::uint64_t radix_anchor_count = 0;
    std::uint64_t radix_passes = 0;
    std::uint64_t radix_histogram_dispatches = 0;
    std::uint64_t radix_scan_dispatches = 0;
    std::uint64_t radix_scatter_dispatches = 0;
    std::uint64_t host_sort_fallbacks = 0;

    /** @brief Compares all accumulated sort counters. */
    [[nodiscard]] constexpr bool
    operator==(const seed_lookup_sort_execution_stats&) const noexcept = default;
};

/**
 * @brief The 24-byte anhor record written by a seed shader.
 *
 * Hashing reads these fields individually so padding bytes do not  affect
 * result comparisons.
 */
struct seed_anchor_wire {
    std::uint64_t ref_position = 0u;
    std::uint64_t query_position = 0u;
    std::uint32_t is_reverse = 0u;
    std::uint8_t query_span = 0u;
    std::uint8_t flags = 0u;
    std::uint16_t segment_id = 0u;
};
static_assert(sizeof(seed_anchor_wire) == 24u);
static_assert(offsetof(seed_anchor_wire, ref_position) == 0u);
static_assert(offsetof(seed_anchor_wire, query_position) == 8u);
static_assert(offsetof(seed_anchor_wire, is_reverse) == 16u);
static_assert(offsetof(seed_anchor_wire, query_span) == 20u);
static_assert(offsetof(seed_anchor_wire, flags) == 21u);
static_assert(offsetof(seed_anchor_wire, segment_id) == 22u);

/** @brief Initial value for the low half of the anchor hash. */
inline constexpr std::uint64_t k_seed_anchor_digest_seed_low = 0x243f6a8885a308d3ULL;
/** @brief Initial value for the high half of the anchor hash */
inline constexpr std::uint64_t k_seed_anchor_digest_seed_high = 0x13198a2e03707344ULL;

/** @brief Anchor-order check and a hash of all anchor fields. */
struct seed_anchor_canonical_order_audit {
    std::uint64_t anchor_count = 0u;
    std::uint64_t digest_lo = k_seed_anchor_digest_seed_low;
    std::uint64_t digest_hi = k_seed_anchor_digest_seed_high;
    bool canonical_order_checked = false;
    bool canonical_order_accepted = false;
};

/** @brief Counts reordered ties and reports whether, target-key order was valid. */
struct seed_anchor_tie_normalization_result {
    std::uint64_t canonical_tie_run_count = 0u;
    std::uint64_t canonical_tie_anchor_count = 0u;
    bool gpu_ref_order_checked = false;
    bool gpu_ref_order_accepted = false;
};

/**
 * @brief Scrambles a field value for the anchor hash using SplitMix steps.
 */
[[nodiscard]] inline constexpr std::uint64_t seed_anchor_digest_mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31u);
}

/**
 * @brief Compares anchors in the order required for chaining.
 *
 * Strand and target position come first. Ties use segment, flags, span, and
 * the low query-position bits, followed by the full query position.
 */
[[nodiscard]] inline constexpr bool
is_seed_anchor_wire_less(const seed_anchor_wire& left, const seed_anchor_wire& right) noexcept {
    constexpr std::uint64_t position_mask = (std::uint64_t{1} << 63u) - 1u;
    const std::uint64_t left_ref =
        (static_cast<std::uint64_t>(left.is_reverse) << 63u) | (left.ref_position & position_mask);
    const std::uint64_t right_ref = (static_cast<std::uint64_t>(right.is_reverse) << 63u) |
                                    (right.ref_position & position_mask);
    if (left_ref != right_ref)
        return left_ref < right_ref;

    const std::uint64_t left_query = (static_cast<std::uint64_t>(left.segment_id) << 48u) |
                                     (static_cast<std::uint64_t>(left.flags) << 40u) |
                                     (static_cast<std::uint64_t>(left.query_span) << 32u) |
                                     static_cast<std::uint32_t>(left.query_position);
    const std::uint64_t right_query = (static_cast<std::uint64_t>(right.segment_id) << 48u) |
                                      (static_cast<std::uint64_t>(right.flags) << 40u) |
                                      (static_cast<std::uint64_t>(right.query_span) << 32u) |
                                      static_cast<std::uint32_t>(right.query_position);
    if (left_query != right_query)
        return left_query < right_query;
    return left.query_position < right.query_position;
}

/**
 * @brief Loads one anchor from a byte payload without alignment assumptions.
 * @param[in] payload Contiguous anchor bytes.
 * @param[in] index Anchor ordinal within `payload`.
 * @return Copied anchor record.
 * @pre `payload` contains the requested complete record.
 */
[[nodiscard]] inline seed_anchor_wire load_seed_anchor_wire(std::span<const std::byte> payload,
                                                            std::uint64_t index) noexcept {
    seed_anchor_wire anchor{};
    std::memcpy(&anchor, payload.data() + index * sizeof(seed_anchor_wire), sizeof(anchor));
    return anchor;
}

/**
 * @brief Stores one anchor into a byte payload without alignment assumptions.
 * @param[out] payload Contiguous anchor bytes to update.
 * @param[in] index Anchor ordinal within `payload`.
 * @param[in] anchor Record copied into the payload.
 * @pre `payload` contains the requested complete record.
 */
inline void store_seed_anchor_wire(std::span<std::byte> payload,
                                   std::uint64_t index,
                                   const seed_anchor_wire& anchor) noexcept {
    std::memcpy(payload.data() + index * sizeof(seed_anchor_wire), &anchor, sizeof(anchor));
}

/**
 * @brief Encodes anchor strand and reference position as the radix-sort key.
 * @param[in] anchor Anchor whose reference key is required.
 * @return Strand in the high bit and reference position in the low bits.
 */
[[nodiscard]] inline constexpr std::uint64_t
seed_anchor_reference_bits(const seed_anchor_wire& anchor) noexcept {
    constexpr std::uint64_t position_mask = (std::uint64_t{1} << 63u) - 1u;
    return (static_cast<std::uint64_t>(anchor.is_reverse) << 63u) |
           (anchor.ref_position & position_mask);
}

/**
 * @brief Swaps two byte-backed anchor records in place.
 * @param[in,out] payload Contiguous anchor bytes to update.
 * @param[in] left First anchor ordinal.
 * @param[in] right Second anchor ordinal.
 * @pre `payload` contains both complete records.
 */
inline void swap_seed_anchor_wires(std::span<std::byte> payload,
                                   std::uint64_t left,
                                   std::uint64_t right) noexcept {
    if (left == right)
        return;
    const seed_anchor_wire left_anchor = load_seed_anchor_wire(payload, left);
    const seed_anchor_wire right_anchor = load_seed_anchor_wire(payload, right);
    store_seed_anchor_wire(payload, left, right_anchor);
    store_seed_anchor_wire(payload, right, left_anchor);
}

/**
 * @brief Sorts an equal-target-position  group by the remaining chaining fields.
 *
 * GPU radix sorting orders the target keys. This in-place heap sort resolves
 * ties using query coordinates and anchor metadata, without allocating another
 * anchor array.
 * @param[in,out] payload Anchor bytes.
 * @param[in] begin First record in the tied group.
 * @param[in] count Number of records in the group.
 */
inline void normalize_seed_anchor_tie_run(std::span<std::byte> payload,
                                          std::uint64_t begin,
                                          std::uint64_t count) noexcept {
    // Restores max-heap order within the selected equal-reference run.
    const auto sift_down = [payload, begin](std::uint64_t heap_count,
                                            std::uint64_t first_root) noexcept {
        std::uint64_t root = first_root;
        while (root < heap_count / 2u) {
            std::uint64_t child = root * 2u + 1u;
            if (child + 1u < heap_count &&
                is_seed_anchor_wire_less(load_seed_anchor_wire(payload, begin + child),
                                         load_seed_anchor_wire(payload, begin + child + 1u))) {
                ++child;
            }
            if (!is_seed_anchor_wire_less(load_seed_anchor_wire(payload, begin + root),
                                          load_seed_anchor_wire(payload, begin + child))) {
                return;
            }
            swap_seed_anchor_wires(payload, begin + root, begin + child);
            root = child;
        }
    };
    for (std::uint64_t root = count / 2u; root != 0u; --root) {
        sift_down(count, root - 1u);
    }
    for (std::uint64_t end = count; end > 1u; --end) {
        swap_seed_anchor_wires(payload, begin, begin + end - 1u);
        sift_down(end - 1u, 0u);
    }
}

/**
 * @brief Checks GPU target-key order and sorts equal-key groups.
 *
 * The payload must contain exactly `anchor_count` records. Invalid strands or
 * decreasing target keys fail the check. Earlier groups may already have been
 * reordered wsen a later group fails. Returns the check status and  tie counts.
 */
[[nodiscard]] inline seed_anchor_tie_normalization_result
normalize_seed_anchor_equal_ref_ties(std::span<std::byte> payload,
                                     std::uint64_t anchor_count) noexcept {
    seed_anchor_tie_normalization_result result{
        .gpu_ref_order_checked = true,
        .gpu_ref_order_accepted = true,
    };
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    if (anchor_count > max / sizeof(seed_anchor_wire) ||
        payload.size() != anchor_count * sizeof(seed_anchor_wire)) {
        result.gpu_ref_order_accepted = false;
        return result;
    }
    std::uint64_t run_begin = 0u;
    while (run_begin < anchor_count) {
        const seed_anchor_wire first = load_seed_anchor_wire(payload, run_begin);
        if (first.is_reverse > 1u) {
            result.gpu_ref_order_accepted = false;
            return result;
        }
        const std::uint64_t run_ref = seed_anchor_reference_bits(first);
        if (run_begin != 0u) {
            const seed_anchor_wire previous = load_seed_anchor_wire(payload, run_begin - 1u);
            if (seed_anchor_reference_bits(previous) > run_ref) {
                result.gpu_ref_order_accepted = false;
                return result;
            }
        }
        std::uint64_t run_end = run_begin + 1u;
        while (run_end < anchor_count) {
            const seed_anchor_wire next = load_seed_anchor_wire(payload, run_end);
            if (next.is_reverse > 1u) {
                result.gpu_ref_order_accepted = false;
                return result;
            }
            const std::uint64_t next_ref = seed_anchor_reference_bits(next);
            if (next_ref < run_ref) {
                result.gpu_ref_order_accepted = false;
                return result;
            }
            if (next_ref != run_ref)
                break;
            ++run_end;
        }
        const std::uint64_t run_count = run_end - run_begin;
        if (run_count > 1u) {
            ++result.canonical_tie_run_count;
            result.canonical_tie_anchor_count += run_count;
            normalize_seed_anchor_tie_run(payload, run_begin, run_count);
        }
        run_begin = run_end;
    }
    return result;
}

/**
 * @brief Checks chaining order and hashes every anchor field.
 *
 * The payload must contain exactly `anchor_count` records. The hash includes
 * record order but excludes padding. Invalid strand flags or decreasing keys
 * fail the order check.
 */
[[nodiscard]] inline seed_anchor_canonical_order_audit
audit_seed_anchor_canonical_order(std::span<const std::byte> payload,
                                  std::uint64_t anchor_count) noexcept {
    seed_anchor_canonical_order_audit audit{
        .anchor_count = anchor_count,
        .canonical_order_checked = true,
        .canonical_order_accepted = true,
    };
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    if (anchor_count > max / sizeof(seed_anchor_wire) ||
        payload.size() != anchor_count * sizeof(seed_anchor_wire)) {
        audit.canonical_order_accepted = false;
        return audit;
    }

    seed_anchor_wire previous{};
    bool have_previous = false;
    for (std::uint64_t index = 0u; index < anchor_count; ++index) {
        seed_anchor_wire anchor{};
        std::memcpy(
            &anchor, payload.data() + index * sizeof(seed_anchor_wire), sizeof(seed_anchor_wire));
        if (anchor.is_reverse > 1u ||
            (have_previous && is_seed_anchor_wire_less(anchor, previous))) {
            audit.canonical_order_accepted = false;
        }

        const std::array<std::uint64_t, 6u> fields{
            anchor.ref_position,
            anchor.query_position,
            anchor.is_reverse,
            anchor.query_span,
            anchor.flags,
            anchor.segment_id,
        };
        for (std::size_t field_index = 0u; field_index < fields.size(); ++field_index) {
            const std::uint64_t tagged = seed_anchor_digest_mix(
                fields[field_index] ^ seed_anchor_digest_mix((index << 3u) | field_index));
            audit.digest_lo =
                seed_anchor_digest_mix(audit.digest_lo ^ tagged ^ 0xa4093822299f31d0ULL);
            audit.digest_hi = std::rotl(audit.digest_hi + tagged + 0x082efa98ec4e6c89ULL, 23);
            audit.digest_hi ^= seed_anchor_digest_mix(tagged ^ 0x452821e638d01377ULL);
        }
        previous = anchor;
        have_previous = true;
    }
    return audit;
}


} // namespace bioseq::gpu::vulkan
