/**
 * @file minimizer_sequence_index/admitted_runs.cppm
 * @brief Reserves memory for minimizer pair runs and sorts them for index construction.
 */
module;

#include <hwy/base.h>

export module bioseq.indexing.minimizer_sequence.admitted_runs;

import std;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.storage.size;

namespace bioseq::indexing::detail {

/**
 * Sort packed hash/position keys in ascending order using the selected SIMD sorter.
 * @param keys Mutable array of key_count packed keys.
 * @param key_count Number of keys.
 * @param sort_implementation Value from pair_run_sort_implementation.
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
 * @brief Owns a vector and the memory reservation that covers its allocation.
 * @tparam p_value_type Vector element type
 */
template <class p_value_type>
struct admitted_vector_run {
    // Members are destroyed in reverse order. Free the vector before making
    // its reserved memory available to other work.
    bioseq::parallelism::memory_permit permit;
    std::uint64_t admitted_bytes = 0;
    std::vector<p_value_type> pairs;

    admitted_vector_run() = default;
    admitted_vector_run(const admitted_vector_run&) = delete;
    admitted_vector_run& operator=(const admitted_vector_run&) = delete;

    admitted_vector_run(admitted_vector_run&& other) noexcept {
        // Transfer the vector while its reservation is still held. Swapping with
        // this empty vector leaves the source with no allocated capacity.
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

    /** Free the vector before releasing its memory reservation. */
    void release_capacity_before_permit() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pairs, permit);
        admitted_bytes = 0;
    }
};

using admitted_pair_run = admitted_vector_run<sorted_pair_t>;

/**
 * Return the vector's allocated element capacity in bytes, saturating on overflow.
 * @tparam p_value_type Vector element type
 */
template <class p_value_type>
[[nodiscard]] std::uint64_t admitted_vector_capacity_bytes(
        const std::vector<p_value_type>& values) noexcept {
    return bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(p_value_type)));
}

/**
 * Estimate the reservation for a vector allocation, including extra capacity
 * the allocator may choose. Return the maximum integer on overflow.
 * @tparam p_value_type Vector element type
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
 * Reserve capacity in an empty vector using an existing memory permit.
 * The permit must cover the planned capacity and transient_bytes for other
 * allocations still in use. After allocation, release any unused reservation.
 * Return false if the vector is not empty or the reservation is insufficient.
 * @tparam p_value_type Vector element type
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
 * Release the reservation for temporary buffers after vector construction.
 * Return false if the vector's allocated capacity exceeds its permit.
 * @tparam p_value_type Vector element type
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
 * Grow the vector after reserving enough memory for its old and new allocations.
 * Return false if the current accounting is invalid or memory cannot be reserved.
 * @tparam p_value_type Vector element type
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
 * Shrink a pair vector that uses at most half its capacity, keeping pair order.
 * Reserve memory for both allocations while copying. If that reservation or
 * the replacement allocation fails, keep the existing vector.
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
    } // The old vector is freed while both allocations are still covered by the permit.
    run.admitted_bytes = replacement_bytes;
    run.permit.shrink_to(replacement_bytes);
}

/// Pair sorter override for this process, used by tests and tuning.
inline std::optional<pair_run_sort_implementation> pair_run_sort_override{};

/** Return the configured pair sorter, defaulting to std_sort. */
[[nodiscard]] inline pair_run_sort_implementation pair_run_sort_impl() noexcept {
    return pair_run_sort_override.value_or(
        pair_run_sort_implementation::std_sort);
}

/** Return the current pair sorter override, if set. */
[[nodiscard]] inline std::optional<pair_run_sort_implementation>
current_pair_run_sort_override() noexcept {
    return pair_run_sort_override;
}

/** Select the pair sorter for this process. */
inline void set_pair_run_sort_impl(pair_run_sort_implementation implementation) noexcept {
    pair_run_sort_override = implementation;
}

/** Restore a saved sorter override, or clear it with nullopt. */
inline void restore_pair_run_sort_override(
    std::optional<pair_run_sort_implementation> implementation) noexcept {
    pair_run_sort_override = implementation;
}

/** Return the diagnostic name of the active pair sorter. */
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
 * Convert max_occ to the threshold used by a direct bucket. A negative value
 * removes the limit. GPU-built buckets add one to a positive threshold.
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

/** Pack the hash above the position so unsigned 128-bit sorting orders both fields. */
[[nodiscard]] inline hwy::uint128_t pack_pair(sorted_pair_t pair) noexcept {
    return hwy::uint128_t{.lo = pair.second, .hi = pair.first};
}

/** Recover the hash and position from a packed sort key. */
[[nodiscard]] inline sorted_pair_t unpack_pair(hwy::uint128_t value) noexcept {
    return sorted_pair_t{value.hi, value.lo};
}

/**
 * Sort pairs through a temporary array of packed 128-bit keys. Reserve its
 * memory first, falling back to std::sort if the reservation is refused.
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

/** Sort hash/position pairs with the active implementation. */
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

/** Limit the collected base count to the range supported by packed GPU positions. */
[[nodiscard]] inline sequence_size_t clamp_gpu_collected_bp(
    sequence_size_t wanted_bp) noexcept
{
    constexpr sequence_size_t max_bp =
        static_cast<sequence_size_t>(minimap_sketch_data::k_position_field_mask);
    return wanted_bp > max_bp ? max_bp : wanted_bp;
}

/**
 * Choose a GPU block size within the packed-position and dispatch limits,
 * allowing for halo bases. Return at least one fragment, even if those limits
 * leave no room for a full fragment.
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
