/**
 * @file minimizer_sequence_index/admitted_runs.cppm
 * @brief Memory-admitted direct minimizer runs and pair sorting.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <hwy/base.h>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:admitted_runs;

import :bucket_codec;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.kmers;
import bioseq.storage.size;

namespace bioseq::indexing::detail {

/**
 * @brief Sorts packed minimizer pairs through the selected SIMD policy.
 * @param keys Mutable packed pair keys
 * @param key_count Number of keys
 * @param sort_implementation Stable sorter ordinal
 */
void sort_minimizer_pair_run_keys(hwy::uint128_t* keys,
                                  std::size_t key_count,
                                  int sort_implementation) noexcept;

} // namespace bioseq::indexing::detail

export namespace bioseq::indexing {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::storage::sequence_size_t;

namespace detail {
using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

/**
 * @brief Couples vector capacity to its memory-admission permit.
 * @tparam p_value_type Vector element type
 */
template <class p_value_type>
struct admitted_vector_run {
    // Reverse member destruction is intentional: retained vector elements and
    // capacity must disappear before the reservation becomes reusable.
    bioseq::parallelism::memory_permit permit;
    std::uint64_t admitted_bytes = 0;
    std::vector<p_value_type> pairs;

    admitted_vector_run() = default;
    admitted_vector_run(const admitted_vector_run&) = delete;
    admitted_vector_run& operator=(const admitted_vector_run&) = delete;

    admitted_vector_run(admitted_vector_run&& other) noexcept {
        // Swap while `other.permit` is still live, then transfer the permit.
        // This gives the moved-from object provably zero capacity.
        pairs.swap(other.pairs);
        admitted_bytes = std::exchange(other.admitted_bytes, 0u);
        permit = std::move(other.permit);
    }

    admitted_vector_run& operator=(admitted_vector_run&& other) noexcept {
        if (this != &other) {
            release_capacity_before_permit();
            pairs.swap(other.pairs);
            admitted_bytes = std::exchange(other.admitted_bytes, 0u);
            permit = std::move(other.permit);
        }
        return *this;
    }

    /** @brief Releases retained vector capacity before releasing its permit. */
    void release_capacity_before_permit() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pairs, permit);
        admitted_bytes = 0;
    }
};

using admitted_pair_run = admitted_vector_run<sorted_pair_t>;

/**
 * @brief Returns the retained byte capacity of an admitted vector.
 * @tparam p_value_type Vector element type
 * @param values Vector to inspect
 * @return Retained capacity in bytes, saturated on overflow
 */
template <class p_value_type>
[[nodiscard]] std::uint64_t admitted_vector_capacity_bytes(
        const std::vector<p_value_type>& values) noexcept {
    return bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(p_value_type)));
}

/**
 * @brief Plans conservative preallocation bytes for a vector.
 * @tparam p_value_type Vector element type
 * @param element_count Requested element count
 * @return Admission requirement, saturated on overflow
 */
template <class p_value_type>
[[nodiscard]] std::uint64_t admitted_vector_preallocation_bytes(
        std::uint64_t element_count) noexcept {
    const std::uint64_t logical_bytes = bioseq::common::saturating_mul(
        element_count, static_cast<std::uint64_t>(sizeof(p_value_type)));
    return bioseq::common::saturating_mul(
        logical_bytes,
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

/**
 * @brief Reserves an empty admitted vector while transient bytes remain live.
 *
 * The caller must pre-acquire
 * `preallocation_headroom + transient_bytes`. After reserve we reconcile to
 * exact retained capacity plus the still-live transient budget.
 *
 * @tparam p_value_type Vector element type
 * @param run Admitted vector and permit
 * @param element_count Requested element capacity
 * @param transient_bytes Additional live bytes retained during allocation
 * @return `true` when the exact capacity remains admitted
 */
template <class p_value_type>
[[nodiscard]] bool reserve_admitted_vector_run_capacity(
        admitted_vector_run<p_value_type>& run,
        std::uint64_t element_count,
        std::uint64_t transient_bytes = 0) {
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    if (!run.pairs.empty() || run.pairs.capacity() != 0u
        || run.admitted_bytes != 0u
        || element_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    const std::uint64_t headroom =
        admitted_vector_preallocation_bytes<p_value_type>(element_count);
    const std::uint64_t required = bioseq::common::saturating_add(
        headroom, transient_bytes);
    if (headroom == max || required == max || !run.permit || run.permit.bytes() < required) {
        return false;
    }

    run.pairs.reserve(static_cast<std::size_t>(element_count));
    const std::uint64_t exact = admitted_vector_capacity_bytes(run.pairs);
    const std::uint64_t exact_with_transient =
        bioseq::common::saturating_add(exact, transient_bytes);
    if (exact == max || exact > headroom || exact_with_transient == max ||
        exact_with_transient > run.permit.bytes()) {
        return false;
    }
    run.admitted_bytes = exact;
    run.permit.shrink_to(exact_with_transient);
    return true;
}

/**
 * @brief Drops transient admission after vector construction.
 * @tparam p_value_type Vector element type
 * @param run Admitted vector and permit
 * @return `true` when retained capacity is fully covered
 */
template <class p_value_type>
[[nodiscard]] bool finish_admitted_vector_run_transient(
        admitted_vector_run<p_value_type>& run) noexcept {
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t exact = admitted_vector_capacity_bytes(run.pairs);
    if (exact == max || (exact != 0u && !run.permit) || exact > run.permit.bytes()) {
        return false;
    }
    run.admitted_bytes = exact;
    run.permit.shrink_to(exact);
    return true;
}

/**
 * @brief Grows an admitted vector without exposing an unreserved allocation.
 * @tparam p_value_type Vector element type
 * @param run Admitted vector and permit
 * @param required_capacity Minimum element capacity
 * @return `true` when the requested capacity is admitted
 */
template <class p_value_type>
[[nodiscard]] bool ensure_admitted_vector_run_capacity(
        admitted_vector_run<p_value_type>& run,
        std::size_t required_capacity) {
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t retained = admitted_vector_capacity_bytes(run.pairs);
    if (retained == max || run.admitted_bytes != retained ||
        (retained != 0u && (!run.permit || run.permit.bytes() < retained))) {
        return false;
    }
    if (run.pairs.capacity() >= required_capacity) {
        return true;
    }
    const std::uint64_t replacement_headroom =
        admitted_vector_preallocation_bytes<p_value_type>(
            static_cast<std::uint64_t>(required_capacity));
    const std::uint64_t allocation_peak = bioseq::common::saturating_add(
        retained, replacement_headroom);
    if (replacement_headroom == max || allocation_peak == max) {
        return false;
    }
    if (run.permit) {
        if (!run.permit.grow_to(allocation_peak)) {
            return false;
        }
    } else {
        if (retained != 0u) {
            return false;
        }
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(allocation_peak);
        if (!permit) {
            return false;
        }
        run.permit = std::move(*permit);
    }
    run.pairs.reserve(required_capacity);
    const std::uint64_t exact = admitted_vector_capacity_bytes(run.pairs);
    if (exact == max || exact > run.permit.bytes()) {
        return false;
    }
    run.admitted_bytes = exact;
    run.permit.shrink_to(exact);
    return true;
}

/**
 * @brief Reduces over-reserved pair capacity without changing pair order.
 *
 * `std::vector` cannot return capacity in place, so the old allocation plus
 * replacement headroom is admitted before constructing the replacement.  If
 * that transient peak is denied, retaining the original admitted allocation is
 * safe and semantically identical.
 */
inline void compact_admitted_pair_run_after_sort(admitted_pair_run& run) {
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t old_bytes = admitted_vector_capacity_bytes(run.pairs);
    if (old_bytes == max || (old_bytes != 0u && !run.permit) || run.admitted_bytes != old_bytes ||
        run.permit.bytes() < old_bytes) {
        index_corruption_panic(
            "minimizers_per_sequence build: pair capacity lacks live admission");
    }
    run.permit.shrink_to(old_bytes);

    const std::uint64_t used_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(run.pairs.size()),
        static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (used_bytes == max || used_bytes > old_bytes / 2u) {
        return;
    }
    const std::uint64_t replacement_headroom =
        admitted_vector_preallocation_bytes<sorted_pair_t>(
            static_cast<std::uint64_t>(run.pairs.size()));
    const std::uint64_t allocation_peak = bioseq::common::saturating_add(
        old_bytes, replacement_headroom);
    if (replacement_headroom == max || allocation_peak == max ||
        !run.permit.grow_to(allocation_peak)) {
        return;
    }

    std::uint64_t replacement_bytes = 0;
    {
        std::vector<sorted_pair_t> compact;
        try {
            compact.reserve(run.pairs.size());
        } catch (const std::bad_alloc&) {
            run.permit.shrink_to(old_bytes);
            return;
        }
        replacement_bytes = admitted_vector_capacity_bytes(compact);
        if (replacement_bytes == max || replacement_bytes > replacement_headroom ||
            bioseq::common::saturating_add(old_bytes, replacement_bytes) > run.permit.bytes()) {
            index_corruption_panic(
                "minimizers_per_sequence build: compacted pair capacity exceeded admission");
        }
        static_assert(std::is_nothrow_move_constructible_v<sorted_pair_t>);
        compact.insert(
            compact.end(),
            std::make_move_iterator(run.pairs.begin()),
            std::make_move_iterator(run.pairs.end()));
        run.pairs.swap(compact);
    } // old pair allocation is destroyed while the peak permit remains live.
    run.admitted_bytes = replacement_bytes;
    run.permit.shrink_to(replacement_bytes);
}

/// @brief Process-local pair sorter override used by tests and tuning.
inline std::optional<pair_run_sort_implementation> pair_run_sort_override{};

/**
 * @brief Returns the active pair-run sorter.
 * @return Configured override or the standard sorter
 */
[[nodiscard]] inline pair_run_sort_implementation pair_run_sort_impl() noexcept {
    return pair_run_sort_override.value_or(
        pair_run_sort_implementation::std_sort);
}

/**
 * @brief Returns the optional pair-run sorter override.
 * @return Current override
 */
[[nodiscard]] inline std::optional<pair_run_sort_implementation>
current_pair_run_sort_override() noexcept {
    return pair_run_sort_override;
}

/**
 * @brief Sets the pair-run sorter override.
 * @param implementation Sorter implementation
 */
inline void set_pair_run_sort_impl(pair_run_sort_implementation implementation) noexcept {
    pair_run_sort_override = implementation;
}

/**
 * @brief Restores an optional pair-run sorter override.
 * @param implementation Override to restore
 */
inline void restore_pair_run_sort_override(
    std::optional<pair_run_sort_implementation> implementation) noexcept {
    pair_run_sort_override = implementation;
}

/**
 * @brief Returns the diagnostic name of the active pair-run sorter.
 * @return Stable sorter name
 */
[[nodiscard]] inline const char* pair_run_sort_impl_name() noexcept {
    switch (pair_run_sort_impl()) {
    case pair_run_sort_implementation::simd_u128_vqsort:
        return "simd_u128_vqsort";
    case pair_run_sort_implementation::simd_u128_vec_stream_1:
        return "simd_u128_vec_stream_1";
    case pair_run_sort_implementation::simd_u128_vec_stream_2:
        return "simd_u128_vec_stream_2";
    case pair_run_sort_implementation::simd_u128_vec_stream_4:
        return "simd_u128_vec_stream_4";
    case pair_run_sort_implementation::std_sort:
    default:
        return "std_sort";
    }
}

/**
 * @brief Derives the direct-bucket occurrence threshold for one route.
 * @param max_occ Requested maximum occurrence count
 * @param is_gpu_built Whether the directory came from GPU generation
 * @return Effective unsigned threshold
 */
[[nodiscard]] constexpr std::uint32_t effective_max_occ_threshold(
    std::int32_t max_occ,
    bool is_gpu_built) noexcept
{
    if (max_occ < 0) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }
    const std::uint32_t threshold = static_cast<std::uint32_t>(max_occ);
    if (!is_gpu_built || threshold == 0u) {
        return threshold;
    }
    const std::uint32_t limit =
        (std::numeric_limits<std::uint32_t>::max)();
    return threshold == limit ? limit : threshold + 1u;
}

/**
 * @brief Packs one sortable hash-position pair into an unsigned 128-bit key.
 * @param pair Hash-position pair
 * @return Packed sortable key
 */
[[nodiscard]] inline hwy::uint128_t pack_pair(sorted_pair_t pair) noexcept {
    return hwy::uint128_t{.lo = pair.second, .hi = pair.first};
}

/**
 * @brief Unpacks one unsigned 128-bit sort key.
 * @param value Packed key
 * @return Hash-position pair
 */
[[nodiscard]] inline sorted_pair_t unpack_pair(hwy::uint128_t value) noexcept {
    return sorted_pair_t{value.hi, value.lo};
}

/**
 * @brief Sorts pairs through an admitted temporary unsigned-128 key vector.
 * @param pairs Hash-position pairs to sort
 * @param implementation SIMD sorter implementation
 */
inline void sort_pair_run_u128(std::vector<sorted_pair_t>& pairs,
                               pair_run_sort_implementation implementation) {
    const std::uint64_t key_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(pairs.size()),
        static_cast<std::uint64_t>(sizeof(hwy::uint128_t)));
    const std::uint64_t key_preallocation_bytes =
        bioseq::common::saturating_mul(
            key_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            key_preallocation_bytes);
    if (!permit) {
        std::sort(pairs.begin(), pairs.end());
        return;
    }

    std::vector<hwy::uint128_t> keys;
    keys.reserve(pairs.size());
    const std::uint64_t exact_key_bytes = admitted_vector_capacity_bytes(keys);
    if (exact_key_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_key_bytes > permit->bytes()) {
        index_corruption_panic(
            "minimizers_per_sequence build: SIMD sort keys exceeded admission");
    }
    permit->shrink_to(exact_key_bytes);
    for (sorted_pair_t p : pairs) {
        keys.push_back(pack_pair(p));
    }
    detail::sort_minimizer_pair_run_keys(
        keys.data(), keys.size(), static_cast<int>(implementation));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        pairs[i] = unpack_pair(keys[i]);
    }
}

/**
 * @brief Sorts one in-memory pair run with the active implementation.
 * @param pairs Hash-position pairs to sort
 */
inline void sort_pair_run(std::vector<sorted_pair_t>& pairs) {
    switch (pair_run_sort_impl()) {
    case pair_run_sort_implementation::simd_u128_vqsort:
        sort_pair_run_u128(pairs, pair_run_sort_implementation::simd_u128_vqsort);
        return;
    case pair_run_sort_implementation::simd_u128_vec_stream_1:
        sort_pair_run_u128(pairs, pair_run_sort_implementation::simd_u128_vec_stream_1);
        return;
    case pair_run_sort_implementation::simd_u128_vec_stream_2:
        sort_pair_run_u128(pairs, pair_run_sort_implementation::simd_u128_vec_stream_2);
        return;
    case pair_run_sort_implementation::simd_u128_vec_stream_4:
        sort_pair_run_u128(pairs, pair_run_sort_implementation::simd_u128_vec_stream_4);
        return;
    case pair_run_sort_implementation::std_sort:
    default:
        std::sort(pairs.begin(), pairs.end());
        return;
    }
}

/**
 * @brief Clamps a GPU collection span to the packed position field.
 * @param wanted_bp Requested collection size
 * @return Clamped collection size
 */
[[nodiscard]] inline sequence_size_t clamp_gpu_collected_bp(
    sequence_size_t wanted_bp) noexcept
{
    constexpr sequence_size_t max_bp =
        static_cast<sequence_size_t>(minimap_sketch_data::k_position_field_mask);
    return wanted_bp > max_bp ? max_bp : wanted_bp;
}

/**
 * @brief Limits a GPU block by position-field and dispatch geometry.
 * @param scalar_fragments_per_block CPU-derived fragment limit
 * @param frag_bp Bases per fragment
 * @param halo_bp Required collection halo
 * @return Safe fragment count for one GPU block
 */
[[nodiscard]] inline std::size_t gpu_fragments_per_block(
    std::size_t scalar_fragments_per_block,
    std::size_t frag_bp,
    std::size_t halo_bp) noexcept
{
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(minimap_sketch_data::k_position_field_mask);
    if (frag_bp == 0u) {
        return 1u;
    }
    if (halo_bp >= max_bp) {
        return 1u;
    }
    constexpr std::size_t gpu_segment_local_size = 128u;
    constexpr std::size_t gpu_segment_max_workgroups = 65535u;
    const std::size_t allowed_core_bp = max_bp - halo_bp;
    const std::size_t by_pos_field =
        std::max<std::size_t>(1u, allowed_core_bp / frag_bp);
    const std::size_t max_dispatch_bp = gpu_segment_local_size * gpu_segment_max_workgroups;
    const std::size_t by_dispatch =
        halo_bp >= max_dispatch_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_dispatch_bp - halo_bp) / frag_bp);
    return std::max<std::size_t>(
        1u,
        std::min({scalar_fragments_per_block, by_pos_field, by_dispatch}));
}

}  // namespace detail

} // namespace bioseq::indexing
