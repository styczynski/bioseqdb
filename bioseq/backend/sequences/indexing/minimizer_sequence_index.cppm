/**
 * @file minimizer_sequence_index.cppm
 * @brief Merged, globally deduplicated minimizers-per-sequence index.
 *
 * The k-mer-hash-range-partitioned implementation is PostgreSQL-free. Its
 * external spill sort is expressed entirely through the storage concept.
 */
module;
#include <libassert/assert.hpp>
#include <hwy/base.h>                      // hwy::uint128_t sort-sidecar ABI
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <expected>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.indexing.minimizer_sequence;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.sequences;
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, minimap_sketch_data, minimizer_sketch_backend
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.lazy_sequence;                          // lazy_sequence
import bioseq.storage.size;                          // sequence_size_t
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.common.hashing;                        // sha256_hasher / sha256_digest (§6 shared digest)
static_assert(__cplusplus >= 202302L, "C++23-or-later");

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
}

export namespace bioseq::indexing {
using bioseq::minimizers::minimap_sketch_data;
using bioseq::simd::collect_minimizers_view;
using bioseq::minimizers::sketch_collector;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::minimizers::minimizer_sketch_backend_name;
using bioseq::storage::sequence_size_t;
using bioseq::storage::lazy_sequence;

/**
 * @brief Records scalar collection work for one direct-bucket build.
 *
 * Every collection leaf records the symbols and packed bytes actually handed
 * to `collect_minimizers_view`. Parallel builds merge worker-local instances
 * after the join in range order.
 */
struct direct_minimizer_bucket_build_stats {
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint32_t letters_per_storage_unit = 0;
    std::uint64_t scalar_collection_calls = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t completed_builds = 0;
    bool backend_bound = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    /**
     * @brief Adds a counter increment with saturation reporting.
     * @param destination Counter to update
     * @param increment Increment to add
     * @param saturated_flag Set when the addition overflows
     */
    static constexpr void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& saturated_flag) noexcept {
        const auto sum = bioseq::common::checked_add(destination, increment);
        if (!sum) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination = *sum;
    }

    /**
     * @brief Binds the receipt to one backend and packed storage shape.
     * @param backend Selected sketch backend
     * @param storage_unit_bytes Bytes per packed storage unit
     * @param letters_per_unit Logical symbols per storage unit
     */
    constexpr void bind_backend(
            minimizer_sketch_backend backend,
            std::uint32_t storage_unit_bytes,
            std::uint32_t letters_per_unit) noexcept {
        if (storage_unit_bytes == 0 || letters_per_unit == 0) {
            aggregation_mismatch = true;
        }
        if (!backend_bound) {
            selected_backend = backend;
            source_storage_unit_bytes = storage_unit_bytes;
            letters_per_storage_unit = letters_per_unit;
            backend_bound = true;
            return;
        }
        aggregation_mismatch = aggregation_mismatch
            || selected_backend != backend
            || source_storage_unit_bytes != storage_unit_bytes
            || letters_per_storage_unit != letters_per_unit;
    }

    /**
     * @brief Records one scalar collection call.
     * @param symbols Logical symbols consumed
     * @param storage_units Packed storage units consumed
     * @param storage_unit_bytes Bytes per storage unit
     * @param letters_per_unit Logical symbols per storage unit
     */
    constexpr void record_scalar_collection(
            std::uint64_t symbols,
            std::uint64_t storage_units,
            std::uint32_t storage_unit_bytes,
            std::uint32_t letters_per_unit) noexcept {
        bind_backend(
            minimizer_sketch_backend::scalar,
            storage_unit_bytes,
            letters_per_unit);
        bool overflow = saturated;
        add(scalar_collection_calls, 1u, overflow);
        add(input_symbols, symbols, overflow);
        add(input_storage_units, storage_units, overflow);
        std::uint64_t bytes = 0;
        const auto byte_count = bioseq::common::checked_mul(
            storage_units, static_cast<std::uint64_t>(storage_unit_bytes));
        if (!byte_count) {
            bytes = (std::numeric_limits<std::uint64_t>::max)();
            overflow = true;
        } else {
            bytes = *byte_count;
        }
        add(input_bytes, bytes, overflow);
        saturated = overflow;
    }

    /**
     * @brief Merges another worker-local receipt.
     * @param other Receipt to merge
     */
    constexpr void merge(
            const direct_minimizer_bucket_build_stats& other) noexcept {
        const bool other_has_counters = other.scalar_collection_calls != 0
            || other.input_symbols != 0
            || other.input_storage_units != 0
            || other.input_bytes != 0
            || other.completed_builds != 0;
        if (!other.backend_bound && !other_has_counters
            && !other.aggregation_mismatch && !other.saturated) {
            return;
        }
        if (other.backend_bound) {
            bind_backend(
                other.selected_backend,
                other.source_storage_unit_bytes,
                other.letters_per_storage_unit);
        } else if (other_has_counters) {
            aggregation_mismatch = true;
        }
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch;
        bool overflow = saturated || other.saturated;
        add(scalar_collection_calls, other.scalar_collection_calls, overflow);
        add(input_symbols, other.input_symbols, overflow);
        add(input_storage_units, other.input_storage_units, overflow);
        add(input_bytes, other.input_bytes, overflow);
        add(completed_builds, other.completed_builds, overflow);
        saturated = overflow;
    }

    /** @brief Records successful completion of the direct-bucket build. */
    constexpr void record_completed_build() noexcept {
        add(completed_builds, 1u, saturated);
    }

    /**
     * @brief Validates backend binding and operation-originated counters.
     * @param require_nonempty_work Whether the build must have consumed input
     * @return `true` when the receipt is internally consistent
     */
    [[nodiscard]] constexpr bool is_internally_consistent(
            bool require_nonempty_work = true) const noexcept {
        if (!backend_bound || selected_backend != minimizer_sketch_backend::scalar
            || aggregation_mismatch || saturated || completed_builds != 1
            || source_storage_unit_bytes == 0
            || letters_per_storage_unit == 0
            || scalar_collection_calls == 0
            || input_storage_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / source_storage_unit_bytes
            || input_bytes
                != input_storage_units * source_storage_unit_bytes
            || input_storage_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / letters_per_storage_unit
            || input_symbols
                > input_storage_units * letters_per_storage_unit) {
            return false;
        }
        return !require_nonempty_work
            || (input_symbols > 0 && input_bytes > 0
                && scalar_collection_calls <= input_symbols);
    }

    [[nodiscard]] constexpr bool operator==(
        const direct_minimizer_bucket_build_stats&) const noexcept = default;
};

/** @brief Hash interval and storage key for one direct minimizer bucket. */
struct direct_minimizer_bucket_directory_entry {
    /// @brief Inclusive smallest hash in the bucket.
    std::uint64_t lo_kmer;
    /// @brief Inclusive largest hash in the bucket.
    std::uint64_t hi_kmer;
    /// @brief Persisted storage sub-key of the bucket.
    std::int64_t sub_key;
};
static_assert(sizeof(direct_minimizer_bucket_directory_entry) == 24 &&
              alignof(direct_minimizer_bucket_directory_entry) == 8);

/**
 * @brief Fixed persisted directory header followed by sorted bucket entries.
 *
 * Field widths, offsets, magic, and layout revision are serialization ABI.
 */
struct direct_minimizer_bucket_directory {
    /// @brief Magic word identifying the direct-bucket directory wire image.
    static constexpr std::uint32_t k_direct_minimizer_bucket_magic = 0xB1000002u;
    /// @brief Current direct-bucket directory layout revision.
    static constexpr std::uint32_t k_direct_minimizer_bucket_layout_revision = 2u;
    /// @brief Flag recording generation through the Vulkan backend.
    static constexpr std::uint32_t k_gpu_built = 1u << 0u;
    /// @brief Persisted magic word.
    std::uint32_t magic = k_direct_minimizer_bucket_magic;
    /// @brief Persisted layout revision.
    std::uint32_t layout_revision = k_direct_minimizer_bucket_layout_revision;
    /// @brief Persisted generation flags.
    std::uint32_t flags = 0;
    /// @brief Reserved field that remains zero in revision 2.
    std::uint32_t reserved = 0;
    /// @brief Number of directory entries following this header.
    std::uint64_t num_buckets = 0;
    /// @brief Total distinct minimizer hashes across all buckets.
    bioseq::storage::sequence_size_t total_distinct_kmers = 0;
    /// @brief Total encoded positions across all buckets.
    bioseq::storage::sequence_size_t total_positions = 0;
};
static_assert(sizeof(direct_minimizer_bucket_directory) == 40 &&
                  alignof(direct_minimizer_bucket_directory) == 8,
              "directory wire header layout pinned");
static_assert(sizeof(bioseq::storage::sequence_size_t) == 8,
              "directory wire fields assume 8-byte sequence_size_t");
static_assert(offsetof(direct_minimizer_bucket_directory, flags) == 8 &&
                  offsetof(direct_minimizer_bucket_directory, reserved) == 12 &&
                  offsetof(direct_minimizer_bucket_directory, num_buckets) == 16 &&
                  offsetof(direct_minimizer_bucket_directory, total_distinct_kmers) == 24 &&
                  offsetof(direct_minimizer_bucket_directory, total_positions) == 32,
              "directory header field offsets pinned");
static_assert(offsetof(direct_minimizer_bucket_directory_entry, sub_key) == 16,
              "dir entry field offsets pinned");

/**
 * @brief Owning aligned directory view supporting binary-search lookup.
 *
 * The source blob is copied so subsequent storage reads cannot invalidate the
 * decoded header or entry pointers.
 */
struct direct_minimizer_bucket_directory_view {
    /// @brief Aligned storage retaining the copied wire bytes.
    std::vector<std::int64_t> owned;
    /// @brief Decoded header within `owned`.
    const direct_minimizer_bucket_directory* hdr = nullptr;
    /// @brief First decoded entry within `owned`.
    const direct_minimizer_bucket_directory_entry* entries = nullptr;
    /// @brief Number of decoded entries.
    std::size_t n = 0;

    /**
     * @brief Copies and validates one persisted directory image.
     * @param blob Persisted bytes
     * @param bytes Persisted byte count
     */
    direct_minimizer_bucket_directory_view(const char* blob, std::size_t bytes) {
        if (bytes < sizeof(direct_minimizer_bucket_directory)) {
            index_corruption_panic("directory: truncated");
        }
        owned.resize((bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t));
        std::memcpy(owned.data(), blob, bytes);
        hdr = reinterpret_cast<const direct_minimizer_bucket_directory*>(owned.data());
        if (hdr->magic != direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic ||
            hdr->layout_revision !=
                direct_minimizer_bucket_directory::k_direct_minimizer_bucket_layout_revision) {
            index_corruption_panic("directory: magic/layout revision");
        }
        n = static_cast<std::size_t>(hdr->num_buckets);
        const std::size_t expected_bytes = sizeof(direct_minimizer_bucket_directory) +
                                           sizeof(direct_minimizer_bucket_directory_entry) * n;
        if (bytes != expected_bytes) {
            index_corruption_panic("directory: size mismatch");
        }
        entries = reinterpret_cast<const direct_minimizer_bucket_directory_entry*>(
            reinterpret_cast<const char*>(owned.data()) +
            sizeof(direct_minimizer_bucket_directory));
        for (std::size_t index = 1; index < n; ++index) {
            if (entries[index].lo_kmer <= entries[index - 1].hi_kmer) {
                index_corruption_panic("directory: not ascending/non-overlapping");
            }
        }
    }
    /**
     * @brief Finds the bucket covering one minimizer hash.
     * @param kmer Minimizer hash
     * @return Matching directory entry or `nullptr`
     */
    [[nodiscard]] const direct_minimizer_bucket_directory_entry* find(std::uint64_t kmer) const {
        std::size_t lo = 0, hi = n;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (entries[mid].hi_kmer < kmer) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < n && entries[lo].lo_kmer <= kmer && kmer <= entries[lo].hi_kmer) {
            return &entries[lo];
        }
        return nullptr;
    }
    /**
     * @brief Reports whether the persisted directory records Vulkan generation.
     * @return `true` when the Vulkan flag is set
     */
    [[nodiscard]] bool is_gpu_built() const noexcept {
        return hdr != nullptr
            && (hdr->flags & direct_minimizer_bucket_directory::k_gpu_built) != 0u;
    }
};

namespace detail {
using encoded_bucket_header = minimizer_hashmap_fragment_index::encoded_header;
using encoded_bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;
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

constexpr std::size_t k_encoded_header_bytes = minimizer_hashmap_fragment_index::k_encoded_header_bytes;     // 32
constexpr std::size_t k_encoded_bucket_bytes = minimizer_hashmap_fragment_index::k_encoded_bucket_bytes;     // 16

/**
 * @brief Serializes a direct-bucket header, records, and flat positions.
 * @param out Destination buffer sized for the complete image
 * @param target_len_val Target length stored in the header
 * @param bkts Bucket records
 * @param poss Flat encoded positions
 */
inline void serialize_direct_minimizer_bucket_image(char* out,
                                                    sequence_size_t target_len_val,
                                                    std::span<const encoded_bucket_record> bkts,
                                                    std::span<const std::uint64_t> poss) {
    const encoded_bucket_header header{minimizer_hashmap_fragment_index::k_encoded_magic,
                                       minimizer_hashmap_fragment_index::k_encoded_layout_revision,
                                       static_cast<std::uint64_t>(target_len_val),
                                       static_cast<std::uint32_t>(bkts.size()),
                                       0u,
                                       static_cast<std::uint64_t>(poss.size())};
    std::memcpy(out, &header, k_encoded_header_bytes);
    if (!bkts.empty()) {
        std::memcpy(
            out + k_encoded_header_bytes, bkts.data(), k_encoded_bucket_bytes * bkts.size());
    }
    if (!poss.empty()) {
        std::memcpy(out + k_encoded_header_bytes + k_encoded_bucket_bytes * bkts.size(),
                    poss.data(),
                    sizeof(std::uint64_t) * poss.size());
    }
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

/** @brief Verifies the qualified spill-merge callback surface. */
struct direct_minimizer_segment_merge_stats {
    template <class p_positions>
    void operator()(
            std::uint32_t,
            std::uint64_t,
            p_positions&& positions) const {
        (void)positions.total_count();
        positions.for_each_batch(
            [](std::span<const std::uint64_t>) noexcept {});
    }
};

/**
 * @brief Checks storage support for qualified direct-minimizer spill replay.
 * @tparam p_storage Candidate spill storage type
 */
template <class p_storage>
concept direct_minimizer_qualified_spill_storage = requires(
        p_storage& storage,
        std::uint32_t position_segment,
        std::uint64_t span,
        std::uint64_t target_length,
        std::span<const detail::sorted_pair_t> run) {
    { storage.begin_segment_spill_generation() }
        -> std::same_as<direct_minimizer_spill_storage_binding>;
    { std::as_const(storage).segment_spill_storage_binding() }
        -> std::same_as<direct_minimizer_spill_storage_binding>;
    { storage.spill_sorted_segment_run(position_segment, run) }
        -> std::convertible_to<std::size_t>;
    { storage.merge_segment_runs(direct_minimizer_segment_merge_stats{}) }
        -> std::same_as<void>;
    { storage.reroute_segment_runs(span, span, target_length) }
        -> std::convertible_to<bool>;
};

template <class p_storage>
class direct_minimizer_external_spill_replay_producer;

template <class p_storage>
class direct_minimizer_unbound_external_spill_replay_producer;

template <class p_storage>
struct direct_minimizer_bucket_index;

/// @brief Names the canonical 32-byte digest of a spill event stream.
using direct_minimizer_content_digest = bioseq::common::sha256_digest;

/**
 * @brief Immutable completion receipt for one persisted direct-bucket generation.
 *
 * Native collection runs exactly once into storage-owned spill runs. Planning
 * and emission replay those same runs, preserving separate route and replay
 * counters so publication cannot infer work from advertised input sizes.
 */
class minimizer_sequence_generation_execution_receipt {
public:
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    std::uint64_t configured_core_span_bp = 0;
    std::uint64_t peak_core_span_bp = 0;
    std::uint64_t peak_core_events = 0;
    std::uint64_t peak_bounded_materialized_window_bp = 0;
    std::uint64_t spill_run_count = 0;
    std::uint64_t spilled_event_count = 0;
    direct_minimizer_content_digest spill_content_digest{};
    std::uint64_t spill_operation_epoch = 0;
    direct_minimizer_spill_storage_binding spill_storage_binding{};
    std::uint64_t spill_qualification_scan_count = 0;
    std::uint64_t spill_qualification_merge_count = 0;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t bounded_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    exact_simd_index_execution_stats exact_simd_stats{};
    bool exact_simd_stats_available = false;
    packed_bounded_nthash_index_status packed_bounded_status =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
    bool packed_bounded_stats_available = false;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
    minimizer_generation_layout generation_layout =
        minimizer_generation_layout::invalid;
    std::uint64_t replay_scan_count = 0;
    std::uint64_t replay_merge_pass_count = 0;
    std::uint64_t replay_reroute_pass_count = 0;
    std::uint64_t replay_completed_scan_count = 0;
    std::uint64_t replay_failed_scan_count = 0;
    std::uint64_t replay_binding_checked_failed_scan_count = 0;
    std::uint64_t replay_illegal_span_transition_count = 0;
    std::uint64_t replay_first_completed_event_count = 0;
    std::uint64_t replay_last_completed_event_count = 0;
    direct_minimizer_content_digest replay_first_completed_content_digest{};
    direct_minimizer_content_digest replay_last_completed_content_digest{};
    std::uint64_t replay_final_span_bp = 0;
    direct_minimizer_spill_storage_binding replay_final_storage_binding{};
    std::uint64_t replay_peak_callback_batch_events = 0;
    std::uint64_t replay_visited_event_count = 0;
    std::uint64_t manifest_position_count = 0;
    std::uint64_t manifest_position_segment_span_bp = 0;
    bool spill_complete = false;
    bool backend_bound = false;
    bool manifest_bound = false;
    bool saturated = false;

    /**
     * @brief Returns the successful replay count required by the chosen layout.
     * @return Required number of completed replays, or zero for an invalid layout
     */
    [[nodiscard]] std::uint64_t
    required_successful_replay_count() const noexcept {
        if (generation_layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return 3u;
        }
        return generation_layout == minimizer_generation_layout::invalid
                || is_supported_minimizer_generation_layout(
                    generation_layout)
            ? 2u : 0u;
    }

    /**
     * @brief Adds a receipt counter with saturation reporting.
     * @param destination Counter to update
     * @param increment Increment to add
     * @param did_saturate Set when the addition overflows
     */
    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        const auto sum = bioseq::common::checked_add(destination, increment);
        if (!sum) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination = *sum;
    }

    /**
     * @brief Validates the sealed spill route and operation-originated counters.
     * @return `true` when the physical spill receipt is self-consistent
     */
    [[nodiscard]] bool is_spill_valid() const noexcept {
        if (!route_snapshot_matches_()
            || !spill_complete || !backend_bound || saturated
            || window_size == 0 || kmer_size == 0
            || configured_core_span_bp == 0
            || peak_core_span_bp > configured_core_span_bp
            || minimizer_scheme
                != minimizer_scheme_identity_for_backend(selected_backend)
            || (spilled_event_count == 0) != (spill_run_count == 0)
            || spill_qualification_scan_count != 1
            || spill_qualification_merge_count
                != (target_length == 0 ? 0u : 1u)
            || authority_seal_ == 0
            || spill_operation_epoch == 0
            || spill_operation_epoch != authority_operation_epoch_
            || spill_storage_binding != authority_storage_binding_
            || spilled_event_count != authority_event_count_
            || spill_content_digest != authority_content_digest_) {
            return false;
        }
        const bool no_exact = exact_simd_core_runs == 0
            && !exact_simd_stats_available;
        const bool no_bounded = bounded_nthash_core_runs == 0
            && !packed_bounded_stats_available;
        const bool no_vulkan = vulkan_core_runs == 0
            && vulkan_stats.completed_roundtrips == 0
            && vulkan_stats.downloaded_minimizers == 0
            && vulkan_stats.gpu_window_splits == 0
            && !vulkan_stats.saturated;
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        const bool require_backend_work = target_length >= minimum_work_bases;
        if (!require_backend_work) {
            return scalar_core_runs == 0 && exact_simd_core_runs == 0
                && bounded_nthash_core_runs == 0 && vulkan_core_runs == 0
                && no_exact && no_bounded && no_vulkan
                && (target_length == 0
                    ? short_core_runs == 0
                    : short_core_runs != 0);
        }
        switch (selected_backend) {
            case minimizer_sketch_backend::scalar:
                return scalar_core_runs != 0
                    && exact_simd_core_runs == 0
                    && bounded_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_bounded && no_vulkan;
            case minimizer_sketch_backend::exact_simd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs != 0
                    && bounded_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && exact_simd_stats_available
                    && exact_simd_stats.is_internally_consistent(true)
                    && no_bounded && no_vulkan;
            case minimizer_sketch_backend::bounded_nthash_simd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && bounded_nthash_core_runs != 0
                    && vulkan_core_runs == 0
                    && no_exact && no_vulkan
                    && packed_bounded_stats_available
                    && packed_bounded_status
                        == packed_bounded_nthash_index_status::ok
                    && packed_bounded_stats.is_internally_consistent(true);
            case minimizer_sketch_backend::vulkan_segment:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && bounded_nthash_core_runs == 0
                    && vulkan_core_runs != 0
                    && no_exact && no_bounded
                    && vulkan_stats.is_internally_consistent(true);
        }
        return false;
    }

    /**
     * @brief Validates spill, replay, manifest, and publication bindings.
     * @return `true` when the completed generation receipt is self-consistent
     */
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const std::uint64_t required_replays =
            required_successful_replay_count();
        if (!is_spill_valid() || !replay_snapshot_matches_()
            || !manifest_bound
            || !manifest_authority_bound_
            || !publication_binding_present_
            || !publication_binding_.valid()
            || generation_layout
                != publication_binding_.layout
            || required_replays == 0u
            || manifest_position_count != spilled_event_count
            || replay_illegal_span_transition_count != 0) {
            return false;
        }
        if (target_length == 0) {
            return replay_scan_count == required_replays
                && replay_merge_pass_count == 0
                && replay_reroute_pass_count == 0
                && replay_completed_scan_count == required_replays
                && replay_failed_scan_count == 0
                && replay_binding_checked_failed_scan_count == 0
                && replay_first_completed_event_count == 0
                && replay_last_completed_event_count == 0
                && replay_first_completed_content_digest
                    == spill_content_digest
                && replay_last_completed_content_digest
                    == spill_content_digest
                && replay_final_span_bp == 1
                && replay_final_storage_binding == spill_storage_binding
                && manifest_position_segment_span_bp == 0;
        }
        return replay_reroute_pass_count
                    <= (std::numeric_limits<std::uint64_t>::max)()
                        - required_replays
            && replay_scan_count
                == replay_reroute_pass_count + required_replays
            && replay_merge_pass_count == replay_scan_count
            && replay_completed_scan_count == required_replays
            && replay_failed_scan_count == replay_reroute_pass_count
            && replay_binding_checked_failed_scan_count
                == replay_failed_scan_count
            && replay_first_completed_event_count == spilled_event_count
            && replay_last_completed_event_count == spilled_event_count
            && replay_first_completed_content_digest
                == replay_last_completed_content_digest
            && (replay_reroute_pass_count != 0
                || replay_first_completed_content_digest
                    == spill_content_digest)
            && replay_final_storage_binding.valid()
            && replay_final_storage_binding.generation_token
                == spill_storage_binding.generation_token
            && spill_storage_binding.mutation_revision
                <= (std::numeric_limits<std::uint64_t>::max)()
                    - replay_reroute_pass_count
            && replay_final_storage_binding.mutation_revision
                == spill_storage_binding.mutation_revision
                    + replay_reroute_pass_count
            && replay_final_span_bp == manifest_position_segment_span_bp
            && replay_final_span_bp != 0;
    }

    /**
     * @brief Checks this receipt against an expected publication binding.
     * @param expected Expected immutable publication identity
     * @return `true` when the completed receipt matches the binding
     */
    [[nodiscard]] bool matches_publication(
            const minimizer_generation_publication_binding& expected)
            const noexcept {
        return is_internally_consistent() && expected.valid()
            && publication_binding_ == expected;
    }

    /**
     * @brief Checks whether the exact replayed spill incarnation may be released.
     * @param before_release Current storage binding before release
     * @return `true` when release preserves the accepted generation authority
     */
    [[nodiscard]] bool allows_ready_release(
            direct_minimizer_spill_storage_binding before_release) const noexcept {
        return is_internally_consistent() && before_release.valid()
            && before_release == replay_final_storage_binding
            && before_release.generation_token
                == spill_storage_binding.generation_token;
    }

private:
    template <class>
    friend class direct_minimizer_external_spill_replay_producer;
    template <class>
    friend struct direct_minimizer_bucket_index;

    // Lifetime-safe capability copied with the receipt. It remains meaningful
    // after the last shared authority owner is destroyed and cannot dangle.
    std::uint64_t authority_seal_ = 0;
    std::uint64_t authority_event_count_ = 0;
    direct_minimizer_content_digest authority_content_digest_{};
    std::uint64_t authority_operation_epoch_ = 0;
    direct_minimizer_spill_storage_binding authority_storage_binding_{};
    bool manifest_authority_bound_ = false;
    minimizer_generation_publication_binding publication_binding_{};
    bool publication_binding_present_ = false;

    struct route_snapshot {
        minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
        minimizer_scheme_identity minimizer_scheme{};
        std::uint64_t target_length = 0;
        std::uint16_t window_size = 0;
        std::uint16_t kmer_size = 0;
        std::uint64_t configured_core_span_bp = 0;
        std::uint64_t peak_core_span_bp = 0;
        std::uint64_t peak_core_events = 0;
        std::uint64_t peak_bounded_materialized_window_bp = 0;
        std::uint64_t spill_run_count = 0;
        std::uint64_t spilled_event_count = 0;
        direct_minimizer_content_digest spill_content_digest{};
        std::uint64_t spill_operation_epoch = 0;
        direct_minimizer_spill_storage_binding spill_storage_binding{};
        std::uint64_t spill_qualification_scan_count = 0;
        std::uint64_t spill_qualification_merge_count = 0;
        std::uint64_t short_core_runs = 0;
        std::uint64_t scalar_core_runs = 0;
        std::uint64_t exact_simd_core_runs = 0;
        std::uint64_t bounded_nthash_core_runs = 0;
        std::uint64_t vulkan_core_runs = 0;
        exact_simd_index_execution_stats exact_simd_stats{};
        bool exact_simd_stats_available = false;
        packed_bounded_nthash_index_status packed_bounded_status =
            packed_bounded_nthash_index_status::invalid_state;
        packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
        bool packed_bounded_stats_available = false;
        bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
        bool spill_complete = false;
        bool backend_bound = false;
        bool saturated = false;
    } route_snapshot_{};
    bool route_snapshot_present_ = false;

    struct replay_snapshot {
        minimizer_generation_layout generation_layout =
            minimizer_generation_layout::invalid;
        std::uint64_t replay_scan_count = 0;
        std::uint64_t replay_merge_pass_count = 0;
        std::uint64_t replay_reroute_pass_count = 0;
        std::uint64_t replay_completed_scan_count = 0;
        std::uint64_t replay_failed_scan_count = 0;
        std::uint64_t replay_binding_checked_failed_scan_count = 0;
        std::uint64_t replay_illegal_span_transition_count = 0;
        std::uint64_t replay_first_completed_event_count = 0;
        std::uint64_t replay_last_completed_event_count = 0;
        direct_minimizer_content_digest replay_first_completed_content_digest{};
        direct_minimizer_content_digest replay_last_completed_content_digest{};
        std::uint64_t replay_final_span_bp = 0;
        direct_minimizer_spill_storage_binding replay_final_storage_binding{};
        std::uint64_t replay_peak_callback_batch_events = 0;
        std::uint64_t replay_visited_event_count = 0;
        std::uint64_t manifest_position_count = 0;
        std::uint64_t manifest_position_segment_span_bp = 0;
        bool manifest_bound = false;
        minimizer_generation_publication_binding publication_binding{};
        bool publication_binding_present = false;
    } replay_snapshot_{};
    bool replay_snapshot_present_ = false;

    [[nodiscard]] static bool same_vulkan_stats_(
            const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& lhs,
            const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& rhs)
            noexcept {
        return lhs.completed_roundtrips == rhs.completed_roundtrips
            && lhs.downloaded_minimizers == rhs.downloaded_minimizers
            && lhs.gpu_window_splits == rhs.gpu_window_splits
            && lhs.saturated == rhs.saturated;
    }

    void capture_route_snapshot_() noexcept {
        route_snapshot_ = route_snapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
            .target_length = target_length,
            .window_size = window_size,
            .kmer_size = kmer_size,
            .configured_core_span_bp = configured_core_span_bp,
            .peak_core_span_bp = peak_core_span_bp,
            .peak_core_events = peak_core_events,
            .peak_bounded_materialized_window_bp =
                peak_bounded_materialized_window_bp,
            .spill_run_count = spill_run_count,
            .spilled_event_count = spilled_event_count,
            .spill_content_digest = spill_content_digest,
            .spill_operation_epoch = spill_operation_epoch,
            .spill_storage_binding = spill_storage_binding,
            .spill_qualification_scan_count = spill_qualification_scan_count,
            .spill_qualification_merge_count = spill_qualification_merge_count,
            .short_core_runs = short_core_runs,
            .scalar_core_runs = scalar_core_runs,
            .exact_simd_core_runs = exact_simd_core_runs,
            .bounded_nthash_core_runs = bounded_nthash_core_runs,
            .vulkan_core_runs = vulkan_core_runs,
            .exact_simd_stats = exact_simd_stats,
            .exact_simd_stats_available = exact_simd_stats_available,
            .packed_bounded_status = packed_bounded_status,
            .packed_bounded_stats = packed_bounded_stats,
            .packed_bounded_stats_available = packed_bounded_stats_available,
            .vulkan_stats = vulkan_stats,
            .spill_complete = spill_complete,
            .backend_bound = backend_bound,
            .saturated = saturated,
        };
        route_snapshot_present_ = true;
    }

    [[nodiscard]] bool route_snapshot_matches_() const noexcept {
        const route_snapshot& value = route_snapshot_;
        return route_snapshot_present_
            && selected_backend == value.selected_backend
            && minimizer_scheme == value.minimizer_scheme
            && target_length == value.target_length
            && window_size == value.window_size
            && kmer_size == value.kmer_size
            && configured_core_span_bp == value.configured_core_span_bp
            && peak_core_span_bp == value.peak_core_span_bp
            && peak_core_events == value.peak_core_events
            && peak_bounded_materialized_window_bp
                == value.peak_bounded_materialized_window_bp
            && spill_run_count == value.spill_run_count
            && spilled_event_count == value.spilled_event_count
            && spill_content_digest == value.spill_content_digest
            && spill_operation_epoch == value.spill_operation_epoch
            && spill_storage_binding == value.spill_storage_binding
            && spill_qualification_scan_count
                == value.spill_qualification_scan_count
            && spill_qualification_merge_count
                == value.spill_qualification_merge_count
            && short_core_runs == value.short_core_runs
            && scalar_core_runs == value.scalar_core_runs
            && exact_simd_core_runs == value.exact_simd_core_runs
            && bounded_nthash_core_runs == value.bounded_nthash_core_runs
            && vulkan_core_runs == value.vulkan_core_runs
            && exact_simd_stats == value.exact_simd_stats
            && exact_simd_stats_available == value.exact_simd_stats_available
            && packed_bounded_status == value.packed_bounded_status
            && packed_bounded_stats == value.packed_bounded_stats
            && packed_bounded_stats_available == value.packed_bounded_stats_available
            && same_vulkan_stats_(vulkan_stats, value.vulkan_stats)
            && spill_complete == value.spill_complete
            && backend_bound == value.backend_bound
            && saturated == value.saturated;
    }

    void capture_replay_snapshot_() noexcept {
        replay_snapshot_ = replay_snapshot{
            .generation_layout = generation_layout,
            .replay_scan_count = replay_scan_count,
            .replay_merge_pass_count = replay_merge_pass_count,
            .replay_reroute_pass_count = replay_reroute_pass_count,
            .replay_completed_scan_count = replay_completed_scan_count,
            .replay_failed_scan_count = replay_failed_scan_count,
            .replay_binding_checked_failed_scan_count =
                replay_binding_checked_failed_scan_count,
            .replay_illegal_span_transition_count =
                replay_illegal_span_transition_count,
            .replay_first_completed_event_count =
                replay_first_completed_event_count,
            .replay_last_completed_event_count =
                replay_last_completed_event_count,
            .replay_first_completed_content_digest =
                replay_first_completed_content_digest,
            .replay_last_completed_content_digest =
                replay_last_completed_content_digest,
            .replay_final_span_bp = replay_final_span_bp,
            .replay_final_storage_binding = replay_final_storage_binding,
            .replay_peak_callback_batch_events =
                replay_peak_callback_batch_events,
            .replay_visited_event_count = replay_visited_event_count,
            .manifest_position_count = manifest_position_count,
            .manifest_position_segment_span_bp =
                manifest_position_segment_span_bp,
            .manifest_bound = manifest_bound,
            .publication_binding = publication_binding_,
            .publication_binding_present = publication_binding_present_,
        };
        replay_snapshot_present_ = true;
    }

    [[nodiscard]] bool replay_snapshot_matches_() const noexcept {
        const replay_snapshot& value = replay_snapshot_;
        return replay_snapshot_present_
            && generation_layout == value.generation_layout
            && replay_scan_count == value.replay_scan_count
            && replay_merge_pass_count == value.replay_merge_pass_count
            && replay_reroute_pass_count == value.replay_reroute_pass_count
            && replay_completed_scan_count == value.replay_completed_scan_count
            && replay_failed_scan_count == value.replay_failed_scan_count
            && replay_binding_checked_failed_scan_count
                == value.replay_binding_checked_failed_scan_count
            && replay_illegal_span_transition_count
                == value.replay_illegal_span_transition_count
            && replay_first_completed_event_count
                == value.replay_first_completed_event_count
            && replay_last_completed_event_count
                == value.replay_last_completed_event_count
            && replay_first_completed_content_digest
                == value.replay_first_completed_content_digest
            && replay_last_completed_content_digest
                == value.replay_last_completed_content_digest
            && replay_final_span_bp == value.replay_final_span_bp
            && replay_final_storage_binding
                == value.replay_final_storage_binding
            && replay_peak_callback_batch_events
                == value.replay_peak_callback_batch_events
            && replay_visited_event_count == value.replay_visited_event_count
            && manifest_position_count == value.manifest_position_count
            && manifest_position_segment_span_bp
                == value.manifest_position_segment_span_bp
            && manifest_bound == value.manifest_bound
            && publication_binding_ == value.publication_binding
            && publication_binding_present_
                == value.publication_binding_present;
    }

    void bind_spill_authority(
            std::uint64_t seal,
            std::uint64_t event_count,
            const direct_minimizer_content_digest& content_digest,
            direct_minimizer_spill_storage_binding storage_binding,
            std::uint64_t operation_epoch) noexcept {
        authority_seal_ = seal;
        authority_event_count_ = event_count;
        authority_content_digest_ = content_digest;
        authority_storage_binding_ = storage_binding;
        authority_operation_epoch_ = operation_epoch;
    }

    [[nodiscard]] bool authority_matches(
            std::uint64_t seal,
            std::uint64_t event_count,
            const direct_minimizer_content_digest& content_digest,
            direct_minimizer_spill_storage_binding storage_binding,
            std::uint64_t operation_epoch) const noexcept {
        return seal != 0 && seal == authority_seal_
            && event_count == authority_event_count_
            && content_digest == authority_content_digest_
            && storage_binding == authority_storage_binding_
            && operation_epoch == authority_operation_epoch_;
    }

    void bind_manifest_authority() noexcept {
        manifest_authority_bound_ = true;
    }

    void bind_publication_authority(
            const encoded_grouped_soa_generation& generation) noexcept {
        publication_binding_ =
            make_minimizer_generation_publication_binding(generation);
        publication_binding_present_ = publication_binding_.valid();
    }

    [[nodiscard]] static std::uint64_t
    next_spill_operation_epoch() noexcept {
        static std::atomic<std::uint64_t> epoch{1u};
        std::uint64_t value = epoch.load(std::memory_order_relaxed);
        for (;;) {
            if (value == 0) return 0;
            const std::uint64_t next = value
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? 0u : value + 1u;
            if (epoch.compare_exchange_weak(
                    value,
                    next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return value;
            }
        }
    }


    [[nodiscard]] static std::uint64_t
    next_spill_authority_seal() noexcept {
        static std::atomic<std::uint64_t> seal{1u};
        std::uint64_t value = seal.load(std::memory_order_relaxed);
        for (;;) {
            if (value == 0) return 0;
            const std::uint64_t next = value
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? 0u : value + 1u;
            if (seal.compare_exchange_weak(
                    value,
                    next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return value;
            }
        }
    }

};

/// Private-construction, immutable authority tying one spill operation to one
/// storage object and the exact canonical event stream qualified from it.
template <class p_storage>
class direct_minimizer_spill_authority {
private:
    template <class>
    friend struct direct_minimizer_bucket_index;
    template <class>
    friend class direct_minimizer_external_spill_replay_producer;

    p_storage* storage_ = nullptr;
    std::uint64_t target_length_ = 0;
    std::uint64_t initial_span_bp_ = 0;
    std::uint64_t run_count_ = 0;
    std::uint64_t event_count_ = 0;
    direct_minimizer_content_digest content_digest_{};
    direct_minimizer_spill_storage_binding storage_binding_{};
    std::uint64_t operation_epoch_ = 0;
    std::uint64_t authority_seal_ = 0;
    minimizer_scheme_identity minimizer_scheme_{};

    direct_minimizer_spill_authority(
            p_storage& storage,
            std::uint64_t target_length,
            std::uint64_t initial_span_bp,
            std::uint64_t run_count,
            std::uint64_t event_count,
            const direct_minimizer_content_digest& content_digest,
            direct_minimizer_spill_storage_binding storage_binding,
            std::uint64_t operation_epoch,
            std::uint64_t authority_seal,
            minimizer_scheme_identity minimizer_scheme) noexcept
        : storage_(&storage),
          target_length_(target_length),
          initial_span_bp_(initial_span_bp),
          run_count_(run_count),
          event_count_(event_count),
          content_digest_(content_digest),
          storage_binding_(storage_binding),
          operation_epoch_(operation_epoch),
          authority_seal_(authority_seal),
          minimizer_scheme_(minimizer_scheme) {}

public:
    direct_minimizer_spill_authority(const direct_minimizer_spill_authority&) = delete;
    direct_minimizer_spill_authority& operator=(const direct_minimizer_spill_authority&) = delete;
    ~direct_minimizer_spill_authority() = default;
};

/// Concrete direct minimizer-bucket producer over PostgreSQL-owned
/// external spill store. The store's bounded k-way merge owns its cursors and
/// batches. This adapter owns no event or run payload. Replaying once per
/// persisted core supplies the common encoder's segment-major canonical order.
template <class p_storage>
class direct_minimizer_external_spill_replay_producer {
private:
    p_storage* storage_ = nullptr;
    minimizer_scheme_identity minimizer_scheme_{};
    std::uint64_t target_length_ = 0;
    std::uint64_t qualified_span_ = 0;
    minimizer_generation_layout generation_layout_ =
        minimizer_generation_layout::invalid;
    std::uint64_t scan_count_ = 0;
    std::uint64_t merge_pass_count_ = 0;
    std::uint64_t reroute_pass_count_ = 0;
    std::uint64_t peak_callback_batch_events_ = 0;
    std::uint64_t visited_event_count_ = 0;
    std::uint64_t completed_scan_count_ = 0;
    std::uint64_t failed_scan_count_ = 0;
    std::uint64_t illegal_span_transition_count_ = 0;
    std::uint64_t first_completed_event_count_ = 0;
    std::uint64_t last_completed_event_count_ = 0;
    direct_minimizer_content_digest first_completed_content_digest_{};
    direct_minimizer_content_digest last_completed_content_digest_{};
    std::uint64_t final_span_bp_ = 0;
    direct_minimizer_spill_storage_binding storage_binding_{};
    direct_minimizer_content_digest expected_replay_digest_{};
    bool expected_replay_digest_valid_ = false;
    std::uint64_t binding_checked_failed_scan_count_ = 0;
    bool has_completed_scan_ = false;
    bool saturated_ = false;
    bool terminal_ = false;
    bool requires_halving_after_failed_scan_ = false;
    minimizer_sequence_generation_execution_receipt spill_receipt_{};
    std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

    void add_counter_(
            std::uint64_t& destination,
            std::uint64_t increment = 1u) noexcept {
        minimizer_sequence_generation_execution_receipt::add(
            destination, increment, saturated_);
    }

    [[nodiscard]] std::uint64_t
    required_successful_replay_count_() const noexcept {
        if (generation_layout_
                == minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return 3u;
        }
        return generation_layout_
                    == minimizer_generation_layout::invalid
                || is_supported_minimizer_generation_layout(
                    generation_layout_)
            ? 2u : 0u;
    }

    void record_completed_scan_(
            std::uint64_t event_count,
            const direct_minimizer_content_digest& content_digest,
            std::uint64_t span) noexcept {
        add_counter_(completed_scan_count_);
        if (!has_completed_scan_) {
            first_completed_event_count_ = event_count;
            first_completed_content_digest_ = content_digest;
            has_completed_scan_ = true;
        }
        last_completed_event_count_ = event_count;
        last_completed_content_digest_ = content_digest;
        final_span_bp_ = span;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    fail_scan_(minimizer_generation_encoding_error error) noexcept {
        add_counter_(failed_scan_count_);
        return std::unexpected(error);
    }

    template <class>
    friend struct direct_minimizer_bucket_index;
    template <class>
    friend class direct_minimizer_unbound_external_spill_replay_producer;

    direct_minimizer_external_spill_replay_producer(
            p_storage& storage,
            std::uint64_t target_length,
            std::uint64_t qualified_span,
            minimizer_scheme_identity minimizer_scheme,
            const minimizer_sequence_generation_execution_receipt& spill_receipt = {},
            std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority = {},
            direct_minimizer_spill_storage_binding storage_binding = {}) noexcept
        : storage_(&storage),
          minimizer_scheme_(minimizer_scheme),
          target_length_(target_length),
          qualified_span_(qualified_span),
          spill_receipt_(spill_receipt),
          authority_(std::move(authority)),
          storage_binding_(storage_binding) {
        if (!storage_binding_.valid()) {
            storage_binding_ = storage.segment_spill_storage_binding();
        }
        if (authority_ != nullptr) {
            expected_replay_digest_ = authority_->content_digest_;
            expected_replay_digest_valid_ = true;
        }
    }

public:
    direct_minimizer_external_spill_replay_producer(
        const direct_minimizer_external_spill_replay_producer&) = delete;
    direct_minimizer_external_spill_replay_producer& operator=(
        const direct_minimizer_external_spill_replay_producer&) = delete;
    direct_minimizer_external_spill_replay_producer(
        direct_minimizer_external_spill_replay_producer&&) = delete;
    direct_minimizer_external_spill_replay_producer& operator=(
        direct_minimizer_external_spill_replay_producer&&) = delete;

    void bind_generation_layout(
            minimizer_generation_layout layout) noexcept {
        if (!is_supported_minimizer_generation_layout(layout)
            || scan_count_ != 0u
            || (generation_layout_
                    != minimizer_generation_layout::invalid
                && generation_layout_ != layout)) {
            saturated_ = true;
            terminal_ = true;
            return;
        }
        generation_layout_ = layout;
    }

    [[nodiscard]] minimizer_scheme_identity
    scheme_identity() const noexcept {
        return minimizer_scheme_;
    }

    /// The persisted physical producer identity comes only from the sealed
    /// native spill receipt. A raw/unbound replay cannot label its recorded
    /// event stream as scalar, SIMD, or Vulkan merely from shared scheme bits.
    [[nodiscard]] minimizer_producer_identity
    producer_backend_identity() const noexcept {
        if (!spill_receipt_.is_spill_valid()) {
            return {};
        }
        const minimizer_producer_identity identity =
            minimizer_producer_backend_identity_for_backend(
                spill_receipt_.selected_backend);
        return minimizer_scheme_identity_for_producer_backend(identity)
                == minimizer_scheme_
            ? identity : minimizer_producer_identity{};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t persisted_span,
            canonical_minimizer_event_receiver receiver) {
        add_counter_(scan_count_);
        if (terminal_ || storage_ == nullptr || persisted_span == 0) {
            return fail_scan_(minimizer_generation_encoding_error::segment_span);
        }
        if (!storage_binding_.valid()
            || storage_->segment_spill_storage_binding()
                != storage_binding_) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (target_length_ == 0) {
            bioseq::common::sha256_hasher empty_hasher;
            const direct_minimizer_content_digest empty_digest = empty_hasher.finalize();
            if (storage_->segment_spill_storage_binding()
                    != storage_binding_) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            if (authority_ != nullptr
                && (authority_->storage_ != storage_
                    || authority_->target_length_ != target_length_
                    || authority_->event_count_ != 0
                    || authority_->content_digest_ != empty_digest
                    || authority_->operation_epoch_ == 0
                    || authority_->storage_binding_ != storage_binding_
                    || authority_->minimizer_scheme_ != minimizer_scheme_
                    || !spill_receipt_.authority_matches(
                        authority_->authority_seal_,
                        0,
                        empty_digest,
                        authority_->storage_binding_,
                        authority_->operation_epoch_))) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            record_completed_scan_(0, empty_digest, persisted_span);
            if (completed_scan_count_
                    == required_successful_replay_count_()) {
                terminal_ = true;
            }
            return {};
        }
        if (persisted_span != qualified_span_) {
            const std::uint64_t expected_next_span = qualified_span_ / 2u;
            if (!requires_halving_after_failed_scan_
                || qualified_span_ <= 1u
                || persisted_span != expected_next_span) {
                add_counter_(illegal_span_transition_count_);
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::segment_span);
            }
            const direct_minimizer_spill_storage_binding before_reroute =
                storage_binding_;
            if (reroute_pass_count_
                    == (std::numeric_limits<std::uint64_t>::max)()
                || !storage_->reroute_segment_runs(
                    qualified_span_, persisted_span, target_length_)) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::position_range);
            }
            const direct_minimizer_spill_storage_binding after_reroute =
                storage_->segment_spill_storage_binding();
            if (!after_reroute.valid()
                || after_reroute.generation_token
                    != before_reroute.generation_token
                || before_reroute.mutation_revision
                    == (std::numeric_limits<std::uint64_t>::max)()
                || after_reroute.mutation_revision
                    != before_reroute.mutation_revision + 1u) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            add_counter_(reroute_pass_count_);
            qualified_span_ = persisted_span;
            storage_binding_ = after_reroute;
            expected_replay_digest_valid_ = false;
            requires_halving_after_failed_scan_ = false;
        } else if (requires_halving_after_failed_scan_) {
            add_counter_(illegal_span_transition_count_);
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::segment_span);
        }
        const std::uint64_t segment_count = target_length_ / persisted_span
            + (target_length_ % persisted_span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::segment_count);
        }
        if (merge_pass_count_
                == (std::numeric_limits<std::uint64_t>::max)()) {
            terminal_ = true;
            return fail_scan_(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        add_counter_(merge_pass_count_);
        std::optional<minimizer_generation_encoding_error> structural_error;
        std::optional<minimizer_generation_encoding_error> receiver_error;
        std::uint64_t scan_event_count = 0;
        bioseq::common::sha256_hasher scan_content_hasher;
        std::uint32_t previous_segment = 0;
        bool have_segment = false;
        storage_->merge_segment_runs(
            [this,
             &structural_error,
             &receiver_error,
             &scan_event_count,
             &scan_content_hasher,
             &previous_segment,
             &have_segment,
             &segment_count,
             &persisted_span,
             &receiver](std::uint32_t segment, std::uint64_t hash, auto&& positions) {
                if (structural_error) {
                    return;
                }
                if (segment >= segment_count
                    || (have_segment && segment < previous_segment)) {
                    structural_error =
                        minimizer_generation_encoding_error::segment_ordinal;
                    return;
                }
                have_segment = true;
                previous_segment = segment;
                positions.for_each_batch(
                    [this,
                     &structural_error,
                     &receiver_error,
                     &scan_event_count,
                     &scan_content_hasher,
                     &hash,
                     &persisted_span,
                     &segment,
                     &receiver](std::span<const std::uint64_t> batch) {
                        peak_callback_batch_events_ = std::max<std::uint64_t>(
                            peak_callback_batch_events_, batch.size());
                        if (structural_error) {
                            return;
                        }
                        for (const std::uint64_t encoded_position : batch) {
                            const std::uint64_t position =
                                encoded_position >> 1u;
                            if (position >= target_length_
                                || position / persisted_span != segment) {
                                structural_error =
                                    minimizer_generation_encoding_error::position_range;
                                return;
                            }
                            if (visited_event_count_
                                    == (std::numeric_limits<std::uint64_t>::max)()) {
                                structural_error =
                                    minimizer_generation_encoding_error::arithmetic_overflow;
                                return;
                            }
                            add_counter_(visited_event_count_);
                            add_counter_(scan_event_count);
                            scan_content_hasher.update_little_endian(hash);
                            scan_content_hasher.update_little_endian(
                                encoded_position);
                            if (!receiver_error) {
                                if (auto accepted = receiver(
                                    canonical_minimizer_event{
                                        .hash = hash,
                                        .encoded_position = encoded_position,
                                    }); !accepted) {
                                    receiver_error = accepted.error();
                                }
                            }
                        }
                    });
            });
        if (storage_->segment_spill_storage_binding() != storage_binding_) {
            structural_error = minimizer_generation_encoding_error::replay_mismatch;
        }
        if (structural_error) {
            terminal_ = true;
            return fail_scan_(*structural_error);
        }
        const direct_minimizer_content_digest content_digest =
            scan_content_hasher.finalize();
        if (authority_ != nullptr
            && (authority_->storage_ != storage_
                || authority_->target_length_ != target_length_
                || authority_->event_count_ != scan_event_count
                || authority_->operation_epoch_ == 0
                || authority_->minimizer_scheme_ != minimizer_scheme_
                || !spill_receipt_.authority_matches(
                    authority_->authority_seal_,
                    authority_->event_count_,
                    authority_->content_digest_,
                    authority_->storage_binding_,
                    authority_->operation_epoch_))) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (expected_replay_digest_valid_) {
            if (content_digest != expected_replay_digest_) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
        } else {
            expected_replay_digest_ = content_digest;
            expected_replay_digest_valid_ = true;
        }
        if (receiver_error) {
            add_counter_(binding_checked_failed_scan_count_);
            if (completed_scan_count_ != 0) {
                terminal_ = true;
            } else {
                requires_halving_after_failed_scan_ = true;
            }
            return fail_scan_(*receiver_error);
        }
        record_completed_scan_(scan_event_count, content_digest, persisted_span);
        if (completed_scan_count_
                == required_successful_replay_count_()) {
            terminal_ = true;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    [[nodiscard]] std::uint64_t merge_pass_count() const noexcept {
        return merge_pass_count_;
    }
    [[nodiscard]] std::uint64_t reroute_pass_count() const noexcept {
        return reroute_pass_count_;
    }
    [[nodiscard]] std::uint64_t peak_callback_batch_events() const noexcept {
        return peak_callback_batch_events_;
    }
    [[nodiscard]] std::uint64_t visited_event_count() const noexcept {
        return visited_event_count_;
    }
    [[nodiscard]] std::uint64_t completed_scan_count() const noexcept {
        return completed_scan_count_;
    }
    [[nodiscard]] std::uint64_t failed_scan_count() const noexcept {
        return failed_scan_count_;
    }
    [[nodiscard]] std::uint64_t
    illegal_span_transition_count() const noexcept {
        return illegal_span_transition_count_;
    }
    [[nodiscard]] minimizer_sequence_generation_execution_receipt
    execution_receipt() const noexcept {
        minimizer_sequence_generation_execution_receipt receipt = spill_receipt_;
        receipt.generation_layout = generation_layout_;
        receipt.replay_scan_count = scan_count_;
        receipt.replay_merge_pass_count = merge_pass_count_;
        receipt.replay_reroute_pass_count = reroute_pass_count_;
        receipt.replay_completed_scan_count = completed_scan_count_;
        receipt.replay_failed_scan_count = failed_scan_count_;
        receipt.replay_binding_checked_failed_scan_count =
            binding_checked_failed_scan_count_;
        receipt.replay_illegal_span_transition_count =
            illegal_span_transition_count_;
        receipt.replay_first_completed_event_count =
            first_completed_event_count_;
        receipt.replay_last_completed_event_count =
            last_completed_event_count_;
        receipt.replay_first_completed_content_digest =
            first_completed_content_digest_;
        receipt.replay_last_completed_content_digest =
            last_completed_content_digest_;
        receipt.replay_final_span_bp = final_span_bp_;
        receipt.replay_final_storage_binding = storage_binding_;
        receipt.replay_peak_callback_batch_events =
            peak_callback_batch_events_;
        receipt.replay_visited_event_count = visited_event_count_;
        receipt.saturated = receipt.saturated || saturated_;
        return receipt;
    }
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

/// Unbound replay adapter for geometry and canonical-order tests. It can
/// exercise retries but never carries a native route snapshot or spill
/// authority, so its receipt cannot satisfy `is_internally_consistent()`.
template <class p_storage>
class direct_minimizer_unbound_external_spill_replay_producer {
private:
    static direct_minimizer_spill_storage_binding binding_for_(p_storage& storage) noexcept {
        direct_minimizer_spill_storage_binding binding =
            storage.segment_spill_storage_binding();
        if (!binding.valid()) {
            binding = storage.begin_segment_spill_generation();
        }
        return binding;
    }

    direct_minimizer_external_spill_replay_producer<p_storage> producer_;

public:
    direct_minimizer_unbound_external_spill_replay_producer(
            p_storage& storage,
            std::uint64_t target_length,
            std::uint64_t qualified_span,
            minimizer_scheme_identity minimizer_scheme) noexcept
        : producer_(
            storage,
            target_length,
            qualified_span,
            minimizer_scheme,
            {},
            {},
            binding_for_(storage)) {}

    direct_minimizer_unbound_external_spill_replay_producer(
        const direct_minimizer_unbound_external_spill_replay_producer&) = delete;
    direct_minimizer_unbound_external_spill_replay_producer& operator=(
        const direct_minimizer_unbound_external_spill_replay_producer&) = delete;
    direct_minimizer_unbound_external_spill_replay_producer(
        direct_minimizer_unbound_external_spill_replay_producer&&) = delete;
    direct_minimizer_unbound_external_spill_replay_producer& operator=(
        direct_minimizer_unbound_external_spill_replay_producer&&) = delete;

    void bind_generation_layout(
            minimizer_generation_layout layout) noexcept {
        producer_.bind_generation_layout(layout);
    }

    [[nodiscard]] minimizer_scheme_identity
    scheme_identity() const noexcept {
        return producer_.scheme_identity();
    }
    [[nodiscard]] minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return producer_.producer_backend_identity();
    }
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t persisted_span,
            canonical_minimizer_event_receiver receiver) {
        return producer_.scan_sorted_events(persisted_span, receiver);
    }
    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return producer_.scan_count();
    }
    [[nodiscard]] std::uint64_t merge_pass_count() const noexcept {
        return producer_.merge_pass_count();
    }
    [[nodiscard]] std::uint64_t reroute_pass_count() const noexcept {
        return producer_.reroute_pass_count();
    }
    [[nodiscard]] std::uint64_t completed_scan_count() const noexcept {
        return producer_.completed_scan_count();
    }
    [[nodiscard]] std::uint64_t failed_scan_count() const noexcept {
        return producer_.failed_scan_count();
    }
    [[nodiscard]] std::uint64_t
    illegal_span_transition_count() const noexcept {
        return producer_.illegal_span_transition_count();
    }
    [[nodiscard]] minimizer_sequence_generation_execution_receipt
    execution_receipt() const noexcept {
        return producer_.execution_receipt();
    }
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

template <class p_storage>
struct direct_minimizer_bucket_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = k_minimizers_per_sequence_index_name;
    using storage_type = p_storage;

    class generation_spill_result {
    private:
        friend struct direct_minimizer_bucket_index<p_storage>;

        std::uint64_t run_count_ = 0;
        exact_simd_index_execution_stats exact_simd_stats_{};
        bool exact_simd_stats_available_ = false;
        minimizer_scheme_identity minimizer_scheme_{};
        std::uint64_t target_length_ = 0;
        std::uint64_t position_segment_span_bp_ = 0;
        minimizer_sequence_generation_execution_receipt execution_receipt_{};
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

        generation_spill_result(
                std::uint64_t run_count,
                const exact_simd_index_execution_stats& exact_simd_stats,
                bool exact_simd_stats_available,
                minimizer_scheme_identity minimizer_scheme,
                std::uint64_t target_length,
                std::uint64_t position_segment_span_bp,
                const minimizer_sequence_generation_execution_receipt& execution_receipt,
                std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority) noexcept
            : run_count_(run_count),
              exact_simd_stats_(exact_simd_stats),
              exact_simd_stats_available_(exact_simd_stats_available),
              minimizer_scheme_(minimizer_scheme),
              target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_receipt_(execution_receipt),
              authority_(std::move(authority)) {}

        void poison_() noexcept {
            run_count_ = 0;
            exact_simd_stats_ = {};
            exact_simd_stats_available_ = false;
            minimizer_scheme_ = {};
            target_length_ = 0;
            position_segment_span_bp_ = 0;
            execution_receipt_ = {};
            authority_.reset();
        }

        void steal_from_(generation_spill_result&& other) noexcept {
            run_count_ = other.run_count_;
            exact_simd_stats_ = other.exact_simd_stats_;
            exact_simd_stats_available_ = other.exact_simd_stats_available_;
            minimizer_scheme_ = other.minimizer_scheme_;
            target_length_ = other.target_length_;
            position_segment_span_bp_ = other.position_segment_span_bp_;
            execution_receipt_ = std::move(other.execution_receipt_);
            authority_ = std::move(other.authority_);
            other.poison_();
        }

    public:
        generation_spill_result() = delete;
        generation_spill_result(const generation_spill_result&) = delete;
        generation_spill_result& operator=(const generation_spill_result&) = delete;
        generation_spill_result(generation_spill_result&& other) noexcept {
            steal_from_(std::move(other));
        }
        generation_spill_result& operator=(generation_spill_result&& other) noexcept {
            if (this != &other) {
                poison_();
                steal_from_(std::move(other));
            }
            return *this;
        }

        [[nodiscard]] std::uint64_t run_count() const noexcept {
            return run_count_;
        }
        [[nodiscard]] const exact_simd_index_execution_stats&
        exact_simd_stats() const noexcept {
            return exact_simd_stats_;
        }
        [[nodiscard]] bool exact_simd_stats_available() const noexcept {
            return exact_simd_stats_available_;
        }
        [[nodiscard]] minimizer_scheme_identity
        minimizer_scheme() const noexcept {
            return minimizer_scheme_;
        }
        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return target_length_;
        }
        [[nodiscard]] std::uint64_t
        position_segment_span_bp() const noexcept {
            return position_segment_span_bp_;
        }
        [[nodiscard]] const minimizer_sequence_generation_execution_receipt&
        execution_receipt() const noexcept {
            return execution_receipt_;
        }
    };

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        segment_unit_range,
        minimizer_generation_encoding_error>
    make_generation_collection_range(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            std::uint64_t core_lo,
            std::uint64_t core_hi,
            std::uint64_t halo_bp = 10u + 15u - 2u) noexcept {
        const std::uint64_t target_length = sequence.bp_size();
        if (core_lo > core_hi || core_hi > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        const std::uint64_t collection_lo = core_lo >= halo_bp
            ? core_lo - halo_bp : 0u;
        const std::uint64_t collection_hi = core_hi
                > target_length - std::min(target_length, halo_bp)
            ? target_length : core_hi + halo_bp;
        if (collection_lo > (std::numeric_limits<sequence_size_t>::max)()
            || collection_hi > (std::numeric_limits<sequence_size_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return segment_unit_range{
            .cs_u = static_cast<sequence_size_t>(collection_lo),
            .ce_u = static_cast<sequence_size_t>(collection_hi),
            .core_lo_bp = static_cast<sequence_size_t>(core_lo),
            .core_hi_bp = static_cast<sequence_size_t>(core_hi),
        };
    }

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        exact_simd_index_execution_stats,
        minimizer_generation_encoding_error>
    collect_generation_core_run(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            std::uint8_t alphabet_code,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            segment_unit_range range,
            sketch_collector& collector,
            std::vector<detail::sorted_pair_t>& output,
            minimizer_sequence_generation_execution_receipt* execution_receipt = nullptr,
            bool allow_packed_bounded = false,
            direct_minimizer_bucket_build_stats* scalar_build_stats = nullptr) {
        const std::uint64_t target_length = sequence.bp_size();
        if ((backend == minimizer_sketch_backend::exact_simd
                && (kernels == nullptr
                    || kernels->two_stack_nthash == nullptr))
            || (backend == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels == nullptr
                    || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (range.core_lo_bp > range.core_hi_bp
            || range.core_hi_bp > target_length
            || range.cs_u > range.ce_u || range.ce_u > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        if (range.core_lo_bp == range.core_hi_bp
            || range.cs_u == range.ce_u) {
            return exact_simd_index_execution_stats{};
        }
        const std::uint64_t emit_base = range.cs_u;
        constexpr std::uint64_t halo_bp = 10u + 15u - 2u;
        const std::uint64_t collection_hi = range.core_hi_bp
                > target_length - std::min(target_length, halo_bp)
            ? target_length : range.core_hi_bp + halo_bp;
        const std::uint64_t local_bp_size = collection_hi > emit_base
            ? collection_hi - emit_base : 0u;
        constexpr std::uint64_t minimum_work_bases = 10u + 15u - 1u;
        const auto add_route = [execution_receipt](
                std::uint64_t minimizer_sequence_generation_execution_receipt::* member,
                std::uint64_t increment = 1u) noexcept {
            if (execution_receipt != nullptr) {
                minimizer_sequence_generation_execution_receipt::add(
                    execution_receipt->*member,
                    increment,
                    execution_receipt->saturated);
            }
        };
        if (local_bp_size < minimum_work_bases) {
            add_route(&minimizer_sequence_generation_execution_receipt::short_core_runs);
            return exact_simd_index_execution_stats{};
        }
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            add_route(&minimizer_sequence_generation_execution_receipt::vulkan_core_runs);
            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats core_stats{};
            auto pairs = bioseq::gpu::vulkan::
                collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    sequence,
                    range.cs_u,
                    range.ce_u,
                    static_cast<sequence_size_t>(emit_base),
                    range.core_lo_bp,
                    range.core_hi_bp,
                    10u,
                    15u,
                    execution_receipt != nullptr ? &core_stats : nullptr);
            if (execution_receipt != nullptr) {
                execution_receipt->vulkan_stats.merge(core_stats);
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->vulkan_stats.saturated;
            }
            output.insert(
                output.end(),
                std::make_move_iterator(pairs.pairs.begin()),
                std::make_move_iterator(pairs.pairs.end()));
            return exact_simd_index_execution_stats{};
        }
        if (backend != minimizer_sketch_backend::scalar
            && backend != minimizer_sketch_backend::exact_simd
            && backend != minimizer_sketch_backend::bounded_nthash_simd) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        collector.clear();
        exact_simd_index_execution_stats exact_simd_stats{};
        if (backend == minimizer_sketch_backend::exact_simd) {
            add_route(&minimizer_sequence_generation_execution_receipt::exact_simd_core_runs);
            exact_simd_stats = collect_exact_simd_index_range_or_throw(
                collector,
                sequence,
                alphabet_code,
                range.cs_u,
                range.ce_u,
                10u,
                15u,
                0u,
                kernels,
                "minimizers_per_sequence build");
            if (execution_receipt != nullptr) {
                execution_receipt->exact_simd_stats.merge(
                    exact_simd_stats);
                execution_receipt->exact_simd_stats_available = true;
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->exact_simd_stats.saturated;
            }
        } else if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
            if (!allow_packed_bounded) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            add_route(&minimizer_sequence_generation_execution_receipt::bounded_nthash_core_runs);
            std::uint64_t materialized_window_bp = 0u;
            const auto bounded_result =
                collect_packed_bounded_nthash_index_lazy(
                    sequence,
                    alphabet_code,
                    range.cs_u,
                    range.ce_u,
                    local_bp_size,
                    10u,
                    15u,
                    kernels,
                    collector,
                    &materialized_window_bp);
            if (execution_receipt != nullptr) {
                execution_receipt->peak_bounded_materialized_window_bp =
                    std::max(
                        execution_receipt->peak_bounded_materialized_window_bp,
                        materialized_window_bp);
                execution_receipt->packed_bounded_status = bounded_result.status;
                execution_receipt->packed_bounded_stats.merge(
                    bounded_result.stats);
                execution_receipt->packed_bounded_stats_available = true;
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->packed_bounded_stats.saturated;
            }
            if (bounded_result.status != packed_bounded_nthash_index_status::ok) {
                throw packed_bounded_nthash_index_runtime_error{
                    bounded_result.status,
                    "minimizers_per_sequence build"};
            }
        } else {
            add_route(&minimizer_sequence_generation_execution_receipt::scalar_core_runs);
            bioseq::simd::collect_minimizers_segment_scalar<
                minimap_sketch_data>(
                    collector, sequence, range.cs_u, range.ce_u, 10u, 15u);
            if (scalar_build_stats != nullptr) {
                scalar_build_stats->record_scalar_collection(
                    range.ce_u - range.cs_u,
                    range.ce_u - range.cs_u,
                    1u,
                    1u);
            }
        }
        for (const minimap_sketch_data& minimizer
             : collector.extract_sketches_data()) {
            const std::uint64_t position = emit_base
                + static_cast<std::uint64_t>(minimizer.last_pos());
            if (position < range.core_lo_bp
                || position >= range.core_hi_bp) {
                continue;
            }
            output.emplace_back(
                minimizer.kmer(),
                (position << 1u)
                    | static_cast<std::uint64_t>(minimizer.strand()));
        }
        return exact_simd_stats;
    }

    /// Prepare external runs serially within the bounded production budget. Each source core is
    /// collected and spilled before the next is allocated. The returned direct minimizer-bucket
    /// producer reopens these same external runs for both encoder passes.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        generation_spill_result,
        minimizer_generation_encoding_error>
    spill_generation_replay_runs_serial(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint64_t core_span_bp = 0,
            std::uint64_t position_segment_span_bp =
                k_minimizer_preferred_segment_span_bp,
            minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr,
            std::uint8_t alphabet_code =
                bioseq::minimizers::k_unbound_packed_alphabet_code) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        constexpr std::uint64_t default_core_span = 32'768u;
        constexpr std::uint64_t lpu = 1u;
        const std::uint64_t target_length = sequence.bp_size();
        struct receipt_output_guard {
            minimizer_sequence_generation_execution_receipt* output = nullptr;
            minimizer_sequence_generation_execution_receipt receipt{};
            ~receipt_output_guard() {
                if (output != nullptr) {
                    *output = receipt;
                }
            }
        } receipt_guard;
        receipt_guard.output = execution_receipt_out;
        minimizer_sequence_generation_execution_receipt& receipt = receipt_guard.receipt;
        receipt.selected_backend = backend;
        receipt.minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        receipt.target_length = target_length;
        receipt.window_size = 10;
        receipt.kmer_size = 15;
        receipt.configured_core_span_bp =
            core_span_bp == 0 ? default_core_span : core_span_bp;
        if ((backend == minimizer_sketch_backend::exact_simd
                && (kernels == nullptr
                    || kernels->two_stack_nthash == nullptr))
            || (backend == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels == nullptr
                    || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (core_span_bp == 0) {
            core_span_bp = default_core_span;
        }
        if (core_span_bp == 0 || core_span_bp > default_core_span
            || position_segment_span_bp == 0) {
            return std::unexpected(minimizer_generation_encoding_error::segment_span);
        }
        const minimizer_scheme_identity minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        if (!is_supported_minimizer_scheme_identity(
                minimizer_scheme.id, minimizer_scheme.revision)) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        direct_minimizer_spill_storage_binding storage_binding =
            storage.begin_segment_spill_generation();
        if (!storage_binding.valid()) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        receipt.backend_bound = true;
        std::uint64_t segment_lo = 0;
        std::uint64_t segment_ordinal = 0;
        {
        struct replay_buffers {
            // Reverse destruction drops both vector owners before admission.
            bioseq::parallelism::memory_permit permit;
            std::vector<detail::sorted_pair_t> run;
            sketch_collector collector;
        } buffers;
        const auto ensure_replay_capacity = [&buffers](std::uint64_t expected_run,
                                                       std::uint64_t expected_sketches) -> bool {
            constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
            if (expected_run
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())
                || expected_sketches
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
                return false;
            }
            const std::uint64_t run_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t sketch_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            const std::uint64_t retained_bytes =
                bioseq::common::saturating_add(run_bytes, sketch_bytes);
            const std::uint64_t run_headroom =
                buffers.run.capacity() < expected_run
                    ? bioseq::common::saturating_mul(
                        bioseq::common::saturating_mul(
                            expected_run,
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::parallelism::k_vector_preallocation_headroom_factor)
                    : 0u;
            const std::uint64_t sketch_headroom =
                buffers.collector.extract_sketches_data().capacity()
                        < expected_sketches
                    ? bioseq::common::saturating_mul(
                        bioseq::common::saturating_mul(
                            expected_sketches,
                            static_cast<std::uint64_t>(
                                sizeof(minimap_sketch_data))),
                        bioseq::parallelism::k_vector_preallocation_headroom_factor)
                    : 0u;
            const std::uint64_t allocation_peak =
                bioseq::common::saturating_add(
                    retained_bytes,
                    bioseq::common::saturating_add(
                        run_headroom, sketch_headroom));
            if (run_bytes == max || sketch_bytes == max || retained_bytes == max ||
                run_headroom == max || sketch_headroom == max || allocation_peak == max) {
                return false;
            }
            if (buffers.permit) {
                if (buffers.permit.bytes() < retained_bytes
                    || !buffers.permit.grow_to(allocation_peak)) {
                    return false;
                }
            } else {
                if (retained_bytes != 0u) {
                    return false;
                }
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(allocation_peak);
                if (!permit) {
                    return false;
                }
                buffers.permit = std::move(*permit);
            }
            if (buffers.run.capacity() < expected_run) {
                buffers.run.reserve(static_cast<std::size_t>(expected_run));
            }
            if (buffers.collector.extract_sketches_data().capacity()
                    < expected_sketches) {
                buffers.collector.reserve(
                    static_cast<std::size_t>(expected_sketches));
            }
            const std::uint64_t exact_run =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t exact_sketch =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            const std::uint64_t exact = bioseq::common::saturating_add(
                exact_run, exact_sketch);
            if (exact_run == max || exact_sketch == max || exact == max ||
                exact > buffers.permit.bytes()) {
                return false;
            }
            buffers.permit.shrink_to(exact);
            return true;
        };

        while (segment_lo < target_length) {
            if (!sql_integer_can_represent(segment_ordinal)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::segment_count);
            }
            const std::uint64_t segment_hi = segment_lo + std::min(
                position_segment_span_bp, target_length - segment_lo);
            for (std::uint64_t core_lo = segment_lo;
                 core_lo < segment_hi;) {
                const std::uint64_t core_hi = core_lo
                    + std::min(core_span_bp, segment_hi - core_lo);
                auto range = make_generation_collection_range(
                    sequence, core_lo, core_hi);
                if (!range) {
                    return std::unexpected(range.error());
                }
                receipt.peak_core_span_bp = std::max(
                    receipt.peak_core_span_bp, core_hi - core_lo);
                buffers.run.clear();
                const std::uint64_t collection_units =
                    static_cast<std::uint64_t>(range->ce_u - range->cs_u);
                const std::uint64_t maximum_new_sketches =
                    bioseq::common::saturating_mul(collection_units, lpu);
                // The checked bounded ntHash kernel's exact-tie matrix can emit the
                // selected minimum plus at most one window span of tied records
                // candidate. Reserve that proven raw-output bound before the
                // kernel runs; canonical sort/unique below reduces it before
                // persistence. Other CPU collectors need at most one retained
                // vector slot per input bp on this bounded core route.
                constexpr std::uint64_t maximum_bounded_records_per_candidate =
                    10u + 1u;
                const std::uint64_t expected_sketches =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? 0u
                        : (backend == minimizer_sketch_backend::bounded_nthash_simd
                            ? bioseq::common::saturating_mul(
                                maximum_new_sketches,
                                maximum_bounded_records_per_candidate)
                            : maximum_new_sketches);
                const std::uint64_t expected_run =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? core_hi - core_lo
                        : expected_sketches;
                if (!ensure_replay_capacity(expected_run, expected_sketches)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                auto collected = collect_generation_core_run(
                    sequence,
                    alphabet_code,
                    backend,
                    kernels,
                    *range,
                    buffers.collector,
                    buffers.run,
                    &receipt,
                    true);
                if (!collected) {
                    return std::unexpected(collected.error());
                }
                const std::uint64_t retained_after_collection =
                    bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(buffers.run.capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(buffers.collector
                                .extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(minimap_sketch_data))));
                if (retained_after_collection
                        == (std::numeric_limits<std::uint64_t>::max)()
                    || retained_after_collection > buffers.permit.bytes()) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                buffers.permit.shrink_to(retained_after_collection);
                detail::sort_pair_run(buffers.run);
                // A source occurrence selected by adjacent tied windows is one
                // persisted event. Canonicalize before spill so qualification,
                // authority receipt, and the encoder all count the same
                // `(hash, encoded_position)` stream. The external merge still
                // deduplicates across arbitrary runs as a defensive invariant.
                buffers.run.erase(
                    std::ranges::unique(buffers.run).begin(),
                    buffers.run.end());
                receipt.peak_core_events = std::max<std::uint64_t>(
                    receipt.peak_core_events, buffers.run.size());
                if (!buffers.run.empty()) {
                    const direct_minimizer_spill_storage_binding before_spill =
                        storage_binding;
                    storage.spill_sorted_segment_run(
                        static_cast<std::uint32_t>(segment_ordinal),
                        std::span<const detail::sorted_pair_t>{buffers.run});
                    storage_binding = storage.segment_spill_storage_binding();
                    if (!storage_binding.valid()
                        || storage_binding.generation_token
                            != before_spill.generation_token
                        || before_spill.mutation_revision
                            == (std::numeric_limits<std::uint64_t>::max)()
                        || storage_binding.mutation_revision
                            != before_spill.mutation_revision + 1u) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::replay_mismatch);
                    }
                    minimizer_sequence_generation_execution_receipt::add(
                        receipt.spill_run_count, 1u, receipt.saturated);
                    minimizer_sequence_generation_execution_receipt::add(
                        receipt.spilled_event_count,
                        buffers.run.size(),
                        receipt.saturated);
                    if (receipt.saturated) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                }
                core_lo = core_hi;
            }
            segment_lo = segment_hi;
            ++segment_ordinal;
        }
        } // replay vectors are destroyed before their shared permit.

        // Qualify the exact order-specific stream now resident in this storage
        // incarnation. Authorized half-reroutes issue a successor mutation
        // revision and adopt a new full SHA-256 only after hashing every event.
        struct spill_qualification {
            std::uint64_t event_count = 0;
            bool saturated = false;
        } qualification;
        auto qualification_producer =
            direct_minimizer_unbound_external_spill_replay_producer<p_storage>{
                storage,
                target_length,
                position_segment_span_bp,
                minimizer_scheme};
        const auto qualified = qualification_producer.scan_sorted_events(
            position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &qualification,
                .emit_fn = +[](void* context,
                               const canonical_minimizer_event& event)
                    -> std::expected<void, minimizer_generation_encoding_error> {
                    auto& state = *static_cast<spill_qualification*>(context);
                    minimizer_sequence_generation_execution_receipt::add(
                        state.event_count, 1u, state.saturated);
                    if (state.saturated) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                    (void)event;
                    return {};
                },
            });
        if (!qualified) {
            return std::unexpected(qualified.error());
        }
        if (qualification.event_count != receipt.spilled_event_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::duplicate_event);
        }
        const minimizer_sequence_generation_execution_receipt qualification_receipt =
            qualification_producer.execution_receipt();
        receipt.spill_content_digest =
            qualification_receipt.replay_first_completed_content_digest;
        receipt.spill_qualification_scan_count =
            qualification_producer.scan_count();
        receipt.spill_qualification_merge_count =
            qualification_producer.merge_pass_count();
        const std::uint64_t operation_epoch =
            minimizer_sequence_generation_execution_receipt::next_spill_operation_epoch();
        const std::uint64_t authority_seal =
            minimizer_sequence_generation_execution_receipt::next_spill_authority_seal();
        if (operation_epoch == 0 || authority_seal == 0) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        receipt.spill_operation_epoch = operation_epoch;
        receipt.spill_storage_binding = storage_binding;
        auto authority = std::shared_ptr<const direct_minimizer_spill_authority<p_storage>>{
            new direct_minimizer_spill_authority<p_storage>{
                storage,
                target_length,
                position_segment_span_bp,
                receipt.spill_run_count,
                receipt.spilled_event_count,
                receipt.spill_content_digest,
                storage_binding,
                operation_epoch,
                authority_seal,
                minimizer_scheme}};
        receipt.bind_spill_authority(
            authority_seal,
            receipt.spilled_event_count,
            receipt.spill_content_digest,
            storage_binding,
            operation_epoch);
        receipt.spill_complete = true;
        receipt.capture_route_snapshot_();
        if (!receipt.is_spill_valid()) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return generation_spill_result{
            receipt.spill_run_count,
            receipt.exact_simd_stats,
            receipt.exact_simd_stats_available,
            minimizer_scheme,
            target_length,
            position_segment_span_bp,
            receipt,
            std::move(authority)};
    }

    [[nodiscard]] static auto make_generation_replay_producer(
            p_storage& storage,
            std::uint64_t target_length,
            generation_spill_result&& spill,
            std::uint64_t position_segment_span_bp =
                k_minimizer_preferred_segment_span_bp) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        minimizer_sequence_generation_execution_receipt receipt = spill.execution_receipt_;
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority =
            std::move(spill.authority_);
        const bool spill_identity_matches =
            authority != nullptr
            && authority->storage_ == &storage
            && authority->target_length_ == target_length
            && authority->initial_span_bp_ == position_segment_span_bp
            && authority->run_count_ == spill.run_count_
            && authority->event_count_ == receipt.spilled_event_count
            && authority->content_digest_ == receipt.spill_content_digest
            && authority->storage_binding_ == receipt.spill_storage_binding
            && storage.segment_spill_storage_binding()
                == authority->storage_binding_
            && authority->operation_epoch_ == receipt.spill_operation_epoch
            && authority->authority_seal_ != 0
            && authority->minimizer_scheme_ == spill.minimizer_scheme_
            && spill.target_length_ == target_length
            && spill.position_segment_span_bp_ == position_segment_span_bp
            && receipt.authority_matches(
                authority->authority_seal_,
                authority->event_count_,
                authority->content_digest_,
                authority->storage_binding_,
                authority->operation_epoch_);
        const bool spill_identity_is_consistent = spill_identity_matches
            && spill.minimizer_scheme_ == receipt.minimizer_scheme
            && spill.run_count_ == receipt.spill_run_count
            && spill.exact_simd_stats_available_
                == receipt.exact_simd_stats_available;
        if (!spill_identity_is_consistent) {
            receipt.backend_bound = false;
            authority.reset();
        }
        const minimizer_scheme_identity producer_scheme =
            spill_identity_is_consistent ? spill.minimizer_scheme_
                                      : minimizer_scheme_identity{};
        spill.poison_();
        return direct_minimizer_external_spill_replay_producer<p_storage>{
            storage, target_length, position_segment_span_bp,
            producer_scheme,
            receipt,
            std::move(authority),
            receipt.spill_storage_binding};
    }

    /// Encode grouped-SoA rows from a sealed direct minimizer-bucket spill. The layout
    /// consumes the sealed direct minimizer-bucket spill without conversion.
    template <minimizer_generation_row_sink p_sink>
    [[nodiscard]] static std::expected<
        encoded_grouped_soa_generation,
        minimizer_generation_encoding_error>
    encode_persisted_generation(
            p_storage& storage,
            p_sink& sink,
            generation_spill_result&& spill,
            const grouped_soa_generation_options& options,
            const grouped_soa_generation_limits& limits = {},
            minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr) {
        if (execution_receipt_out != nullptr) {
            *execution_receipt_out = {};
        }
        if (options.layout
                != minimizer_generation_layout::grouped_soa_segment_local
            || options.window_size != 10 || options.kmer_size != 15
            || spill.position_segment_span_bp_
                != k_minimizer_preferred_segment_span_bp
            || spill.authority_ == nullptr
            || spill.authority_->storage_ != &storage
            || storage.segment_spill_storage_binding()
                != spill.authority_->storage_binding_
            || !spill.execution_receipt_.is_spill_valid()) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        auto producer = make_generation_replay_producer(
            storage,
            options.target_length,
            std::move(spill),
            k_minimizer_preferred_segment_span_bp);
        auto encoded = encode_minimizer_generation(
            producer, sink, options, limits);
        minimizer_sequence_generation_execution_receipt receipt =
            producer.execution_receipt();
        if (encoded) {
            receipt.manifest_position_count =
                encoded->manifest.position_count;
            receipt.manifest_position_segment_span_bp =
                encoded->manifest.position_segment_span_bp;
            receipt.manifest_bound =
                encoded->manifest.layout == options.layout
                && encoded->manifest.layout
                    == receipt.generation_layout
                && encoded->manifest.target_length == receipt.target_length
                && encoded->manifest.position_count
                    == receipt.spilled_event_count
                && encoded->manifest.kmer_size == receipt.kmer_size
                && encoded->manifest.window_size == receipt.window_size
                && encoded->manifest.minimizer_scheme_id
                    == receipt.minimizer_scheme.id
                && encoded->manifest.minimizer_scheme_revision
                    == receipt.minimizer_scheme.revision
                && encoded->manifest.producer_backend_id
                    == minimizer_producer_backend_identity_for_backend(
                        receipt.selected_backend).id
                && encoded->manifest.producer_identity_revision
                    == k_minimizer_producer_identity_revision;
            if (receipt.manifest_bound) {
                receipt.bind_manifest_authority();
                receipt.bind_publication_authority(*encoded);
                receipt.capture_replay_snapshot_();
            }
            if (!receipt.is_internally_consistent()) {
                encoded = std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
        }
        if (execution_receipt_out != nullptr) {
            *execution_receipt_out = receipt;
        }
        return encoded;
    }

    template <sequence_alphabet p_alphabet>
    static void build(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        direct_minimizer_bucket_build_stats* scalar_build_stats_out = nullptr,
        std::uint8_t alphabet_code =
            bioseq::minimizers::k_unbound_packed_alphabet_code) {
        constexpr std::size_t w = 10, k = 15;
        constexpr std::size_t lpu = 1u;
        constexpr std::size_t frag_bp    = 32'768u;
        constexpr std::size_t window_span = w + k - 2u;
        static_assert(frag_bp < (std::size_t{1} << 28), "fragment core positions fit 28-bit field");
        using detail::k_encoded_header_bytes; using detail::k_encoded_bucket_bytes; using detail::encoded_bucket_record;
        constexpr std::size_t k_bucket_target_bytes = std::size_t{8} << 20;       // 8 MiB

        if (backend == minimizer_sketch_backend::exact_simd
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_sequence build"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_sequence build"};
        }

        if (scalar_build_stats_out != nullptr) {
            *scalar_build_stats_out = {};
        }
        const bool capture_scalar_build_stats =
            scalar_build_stats_out != nullptr;
        direct_minimizer_bucket_build_stats scalar_build_stats{};
        if (capture_scalar_build_stats) {
            scalar_build_stats.bind_backend(
                backend,
                1u,
                1u);
        }

        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

        // ---- Pass 0: collect globalised (kmer, encoded) pairs, spill sorted runs.
        const std::size_t run_buffer_bytes = storage.run_buffer_bytes();
        using pair_t = detail::sorted_pair_t;
        using detail::admitted_pair_run;
        const auto make_core_range = [&seq](sequence_size_t core_lo, sequence_size_t core_hi) {
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, window_span);
            if (!range) {
                index_corruption_panic(
                    "minimizers_per_sequence build: invalid collection/core range");
            }
            return *range;
        };
        struct worker_range_result {
            detail::admitted_vector_run<admitted_pair_run> runs;
            detail::admitted_vector_run<segment_unit_range>
                deferred_for_backend_spill;
            direct_minimizer_bucket_build_stats scalar_build_stats{};
            std::exception_ptr error;
            segment_unit_range error_range{};
        };

        auto release_untransferred_results_noexcept =
            [](std::vector<worker_range_result>& results) noexcept {
                for (worker_range_result& result : results) {
                    for (admitted_pair_run& run : result.runs.pairs) {
                        run.release_capacity_before_permit();
                    }
                    result.runs.release_capacity_before_permit();
                    result.deferred_for_backend_spill
                        .release_capacity_before_permit();
                }
            };

        // under vulkan_segment the fused GPU sketch emits the
        // window's minimizers on-device; the globalised (kmer, encoded_pos)
        // pairs are sorted on the HOST, so there is no CPU minimizer COLLECT on
        // this path. The scalar branch is unchanged.
        auto collect_core_range_pairs = [&seq, backend, kernels, alphabet_code](
                                            sequence_size_t core_lo,
                                            sequence_size_t core_hi,
                                            sketch_collector& collector,
                                            std::vector<pair_t>& out,
                                            direct_minimizer_bucket_build_stats* scalar_stats) {
            if (core_hi <= core_lo) {
                return;
            }
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, window_span);
            if (!range || !collect_generation_core_run(
                    seq,
                    alphabet_code,
                    backend,
                    kernels,
                    *range,
                    collector,
                    out,
                    nullptr,
                    true,
                    scalar_stats)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collection/core geometry failed");
            }
        };

        auto collect_sort_and_spill_backend_scratch =
            [&backend,
             &storage,
             &make_core_range,
             &collect_core_range_pairs,
             &capture_scalar_build_stats,
             &scalar_build_stats](auto& self, segment_unit_range range) -> void {
            const std::uint64_t bp =
                static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
            constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
            const std::uint64_t maximum_output_pairs =
                backend == minimizer_sketch_backend::bounded_nthash_simd
                    ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                    : bp;
            const std::uint64_t pair_preallocation_bytes =
                detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
            // The strict Vulkan segment leaf returns an owned pair run
            // carrying its single scratch/result permit. Do not reserve
            // that same GPU transaction again in this caller.
            const std::uint64_t backend_scratch_bytes = 0u;
            const std::uint64_t maximum_collection_candidates =
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                                               static_cast<std::uint64_t>(lpu));
            const std::uint64_t maximum_collector_sketches =
                backend == minimizer_sketch_backend::vulkan_segment
                    ? 0u
                    : (backend == minimizer_sketch_backend::bounded_nthash_simd
                           ? bioseq::common::saturating_mul(maximum_collection_candidates,
                                                            maximum_bounded_records_per_candidate)
                           : maximum_collection_candidates);
            const std::uint64_t collector_headroom_bytes =
                detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                    maximum_collector_sketches);
            const std::uint64_t admission_bytes = bioseq::common::saturating_add(
                pair_preallocation_bytes,
                bioseq::common::saturating_add(collector_headroom_bytes, backend_scratch_bytes));
            if (backend == minimizer_sketch_backend::vulkan_segment) {
                if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                  s.demote_resident_runs_until_available(bytes);
                              }) {
                    storage.demote_resident_runs_until_available(admission_bytes);
                }
            }
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                admission_bytes);
            if (!permit) {
                if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                  s.demote_resident_runs_until_available(bytes);
                              }) {
                    storage.demote_resident_runs_until_available(admission_bytes);
                    permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            admission_bytes);
                }
            }
            if (!permit) {
                constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
                constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
                const sequence_size_t min_core_bp =
                    backend == minimizer_sketch_backend::vulkan_segment ? k_vulkan_minimum_core_bp
                                                                        : k_scalar_minimum_core_bp;
                if (range.core_hi_bp - range.core_lo_bp > min_core_bp) {
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid));
                    self(self, make_core_range(mid, range.core_hi_bp));
                    return;
                }
                // Never bypass the configured cap with an "emergency"
                // heap vector.  The caller can retry after pressure clears.
                throw std::bad_alloc{};
            }
            admitted_pair_run scratch;
            scratch.permit = std::move(*permit);
            if (!detail::reserve_admitted_vector_run_capacity(
                    scratch,
                    maximum_output_pairs,
                    bioseq::common::saturating_add(collector_headroom_bytes,
                                                   backend_scratch_bytes))) {
                index_corruption_panic(
                    "minimizers_per_sequence build: scratch pair capacity exceeded admission");
            }
            {
                // Declared after `scratch`: collector capacity is destroyed
                // before the shared permit can shrink or unwind.
                sketch_collector collector;
                if (maximum_collector_sketches != 0u) {
                    if (maximum_collector_sketches >
                        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: collector capacity overflow");
                    }
                    collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
                }
                const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                    scratch.admitted_bytes,
                    bioseq::common::saturating_add(exact_collector_bytes, backend_scratch_bytes));
                if (exact_collector_bytes > collector_headroom_bytes ||
                    live_before_collect > scratch.permit.bytes()) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collector capacity exceeded admission");
                }
                scratch.permit.shrink_to(live_before_collect);
                collect_core_range_pairs(range.core_lo_bp,
                                         range.core_hi_bp,
                                         collector,
                                         scratch.pairs,
                                         capture_scalar_build_stats ? &scalar_build_stats
                                                                    : nullptr);
                const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                    detail::admitted_vector_capacity_bytes(scratch.pairs),
                    bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(
                                collector.extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                        backend_scratch_bytes));
                if (live_after_collect > scratch.permit.bytes()) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collection grew beyond admission");
                }
            }
            // Backend scratch has returned; retain only exact pair capacity.
            if (!detail::finish_admitted_vector_run_transient(scratch)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: scratch pair collection exceeded admission");
            }
            if (backend != minimizer_sketch_backend::vulkan_segment) {
                detail::sort_pair_run(scratch.pairs);
            }
            storage.spill_sorted_run(
                std::span<const pair_t>(scratch.pairs.data(), scratch.pairs.size()));
            scratch.release_capacity_before_permit();
        };

        auto transfer_result_runs = [&capture_scalar_build_stats,
                                     &scalar_build_stats,
                                     &storage,
                                     &collect_sort_and_spill_backend_scratch,
                                     &release_untransferred_results_noexcept](
                                        std::vector<worker_range_result>& results) {
            for (worker_range_result& result : results) {
                if (result.error) {
                    try {
                        std::rethrow_exception(result.error);
                    } catch (const exact_simd_index_runtime_error&) {
                        // Preserve the checked two_stack_nthash failure so the PG
                        // SQL boundary can translate it to 0A000. Converting
                        // it to index_corruption_panic would abort the backend
                        // and hide the original unsupported shape/status.
                        throw;
                    } catch (const packed_bounded_nthash_index_runtime_error&) {
                        // Capability and memory-pressure rejection is an
                        // operation failure, never persisted-index corruption.
                        throw;
                    } catch (const std::exception& e) {
                        const std::string message =
                            "bioseq minimizers_per_sequence worker failed for core=["
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_lo_bp))
                            + ","
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_hi_bp))
                            + "): "
                            + e.what();
                        index_corruption_panic(message);
                    } catch (...) {
                        const std::string message =
                            "bioseq minimizers_per_sequence worker failed for core=["
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_lo_bp))
                            + ","
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_hi_bp))
                            + "): non-std exception";
                        index_corruption_panic(message);
                    }
                }
                if (capture_scalar_build_stats) {
                    scalar_build_stats.merge(result.scalar_build_stats);
                }
                for (admitted_pair_run& run : result.runs.pairs) {
                    if (!run.pairs.empty()) {
                        storage.spill_sorted_run_owned_admitted(
                            std::move(run.pairs),
                            run.admitted_bytes,
                            std::move(run.permit));
                        run.admitted_bytes = 0;
                    }
                }
                for (segment_unit_range range
                     : result.deferred_for_backend_spill.pairs) {
                    collect_sort_and_spill_backend_scratch(
                        collect_sort_and_spill_backend_scratch, range);
                }
            }
            release_untransferred_results_noexcept(results);
        };

        auto collect_range_admitted =
            [&backend,
             &storage,
             &make_core_range,
             &collect_core_range_pairs,
             &capture_scalar_build_stats](
                auto& self, segment_unit_range range, worker_range_result& result) -> void {
            if (result.error) {
                return;
            }
            try {
                const std::uint64_t bp =
                    static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
                constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
                const std::uint64_t maximum_output_pairs =
                    backend == minimizer_sketch_backend::bounded_nthash_simd
                        ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                        : bp;
                const std::uint64_t pair_preallocation_bytes =
                    detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
                const std::uint64_t backend_scratch_bytes = 0u;
                const std::uint64_t maximum_collection_candidates = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                    static_cast<std::uint64_t>(lpu));
                const std::uint64_t maximum_collector_sketches =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? 0u
                        : (backend == minimizer_sketch_backend::bounded_nthash_simd
                               ? bioseq::common::saturating_mul(
                                     maximum_collection_candidates,
                                     maximum_bounded_records_per_candidate)
                               : maximum_collection_candidates);
                const std::uint64_t collector_headroom_bytes =
                    detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                        maximum_collector_sketches);
                const std::uint64_t admission_bytes = bioseq::common::saturating_add(
                    pair_preallocation_bytes,
                    bioseq::common::saturating_add(collector_headroom_bytes,
                                                   backend_scratch_bytes));
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                      s.demote_resident_runs_until_available(bytes);
                                  }) {
                        storage.demote_resident_runs_until_available(admission_bytes);
                    }
                }
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        admission_bytes);
                if (!permit) {
                    constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
                    constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
                    const sequence_size_t min_core_bp =
                        backend == minimizer_sketch_backend::vulkan_segment
                            ? k_vulkan_minimum_core_bp
                            : k_scalar_minimum_core_bp;
                    if (range.core_hi_bp - range.core_lo_bp <= min_core_bp) {
                        if (!detail::ensure_admitted_vector_run_capacity(
                                result.deferred_for_backend_spill,
                                result.deferred_for_backend_spill.pairs.size() + 1u)) {
                            throw std::bad_alloc{};
                        }
                        result.deferred_for_backend_spill.pairs.push_back(range);
                        return;
                    }
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid), result);
                    self(self, make_core_range(mid, range.core_hi_bp), result);
                    return;
                }

                admitted_pair_run run{};
                run.permit = std::move(*permit);
                if (!detail::reserve_admitted_vector_run_capacity(
                        run,
                        maximum_output_pairs,
                        bioseq::common::saturating_add(collector_headroom_bytes,
                                                       backend_scratch_bytes))) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: worker pair capacity exceeded admission");
                }
                {
                    sketch_collector collector;
                    if (maximum_collector_sketches != 0u) {
                        if (maximum_collector_sketches >
                            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: worker collector overflow");
                        }
                        collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
                    }
                    const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                        static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                    const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                        run.admitted_bytes,
                        bioseq::common::saturating_add(exact_collector_bytes,
                                                       backend_scratch_bytes));
                    if (exact_collector_bytes > collector_headroom_bytes ||
                        live_before_collect > run.permit.bytes()) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: worker collector exceeded admission");
                    }
                    run.permit.shrink_to(live_before_collect);
                    collect_core_range_pairs(range.core_lo_bp,
                                             range.core_hi_bp,
                                             collector,
                                             run.pairs,
                                             capture_scalar_build_stats ? &result.scalar_build_stats
                                                                        : nullptr);
                    const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                        detail::admitted_vector_capacity_bytes(run.pairs),
                        bioseq::common::saturating_add(
                            bioseq::common::saturating_mul(
                                static_cast<std::uint64_t>(
                                    collector.extract_sketches_data().capacity()),
                                static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                            backend_scratch_bytes));
                    if (live_after_collect > run.permit.bytes()) {
                        index_corruption_panic("minimizers_per_sequence build: worker collection "
                                               "grew beyond admission");
                    }
                }
                // Backend scratch has returned; only vector capacity remains.
                if (!detail::finish_admitted_vector_run_transient(run)) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: worker pair collection exceeded admission");
                }
                if (backend != minimizer_sketch_backend::vulkan_segment) {
                    detail::sort_pair_run(run.pairs);
                }
                detail::compact_admitted_pair_run_after_sort(run);
                if (!detail::ensure_admitted_vector_run_capacity(result.runs,
                                                                 result.runs.pairs.size() + 1u)) {
                    throw std::bad_alloc{};
                }
                result.runs.pairs.push_back(std::move(run));
            } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
                const std::string_view message{err.what()};
                const bool split_admission_decline =
                    message.starts_with("Vulkan segment minimizer pair build returned invalid ");
                if (backend == minimizer_sketch_backend::vulkan_segment &&
                    split_admission_decline) {
                    constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                    if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                        const sequence_size_t mid =
                            range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid), result);
                        self(self, make_core_range(mid, range.core_hi_bp), result);
                        return;
                    }
                }
                result.error = std::current_exception();
                result.error_range = range;
            } catch (const std::bad_alloc&) {
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                    if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                        const sequence_size_t mid =
                            range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid), result);
                        self(self, make_core_range(mid, range.core_hi_bp), result);
                        return;
                    }
                }
                result.error = std::current_exception();
                result.error_range = range;
            } catch (...) {
                result.error = std::current_exception();
                result.error_range = range;
            }
        };

        const std::uint64_t peak_bytes_per_fragment =
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(frag_bp),
                static_cast<std::uint64_t>(sizeof(pair_t)));
        const std::size_t scalar_fragments_per_block = std::max<std::size_t>(
            1,
            run_buffer_bytes / std::max<std::uint64_t>(1, peak_bytes_per_fragment));
        // one GPU window's local positions must fit the 28-bit
        // field, so clamp the per-block fragment count when collecting on GPU.
        const std::size_t fragments_per_block =
            backend == minimizer_sketch_backend::vulkan_segment
                ? detail::gpu_fragments_per_block(
                    scalar_fragments_per_block,
                    frag_bp,
                    2u * window_span)
                : scalar_fragments_per_block;
        detail::admitted_vector_run<segment_unit_range> admitted_window_ranges;
        const std::size_t maximum_window_ranges =
            nfrag == 0u
                ? 0u
                : (nfrag + fragments_per_block - 1u) / fragments_per_block;
        if (!detail::ensure_admitted_vector_run_capacity(
                admitted_window_ranges, maximum_window_ranges)) {
            index_corruption_panic(
                "minimizers_per_sequence build: window metadata admission failed");
        }
        std::vector<segment_unit_range>& window_ranges =
            admitted_window_ranges.pairs;
        for (std::size_t frag_lo = 0; frag_lo < nfrag; frag_lo += fragments_per_block) {
            const std::size_t frag_hi = std::min(nfrag, frag_lo + fragments_per_block);
            const sequence_size_t core_lo = static_cast<sequence_size_t>(frag_lo) * frag_bp;
            const sequence_size_t core_hi = std::min(
                static_cast<sequence_size_t>(frag_hi) * frag_bp, total_bp);
            if (core_hi > core_lo) {
                window_ranges.push_back(make_core_range(core_lo, core_hi));
            }
        }

        auto collect_serial = [&window_ranges, &collect_range_admitted, &transfer_result_runs]() {
            detail::admitted_vector_run<worker_range_result> admitted_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_results, 1u)) {
                throw std::bad_alloc{};
            }
            admitted_results.pairs.resize(1u);
            std::vector<worker_range_result>& results = admitted_results.pairs;
            for (segment_unit_range range : window_ranges) {
                results[0] = worker_range_result{};
                collect_range_admitted(collect_range_admitted, range, results[0]);
                transfer_result_runs(results);
            }
        };

        auto& pool = bioseq::parallelism::global_pool::instance();
        // Vulkan serialises window submissions through one runtime queue. Both
        // host collectors are independent per range when the lazy scanner
        // advertises worker sharing.
        const bool may_parallel_collect =
            backend != minimizer_sketch_backend::vulkan_segment
            && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
            && pool.maximum_concurrency() > 1
            && bioseq::storage::is_worker_shareable(seq)
            && nfrag
                >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;

        if (!may_parallel_collect) {
            collect_serial();
        } else {
            detail::admitted_vector_run<worker_range_result>
                admitted_local_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_local_results, window_ranges.size())) {
                throw std::bad_alloc{};
            }
            admitted_local_results.pairs.resize(window_ranges.size());
            std::vector<worker_range_result>& local_results =
                admitted_local_results.pairs;
            try {
                bioseq::parallelism::parallel_for_pipeline(
                    0, local_results.size(), 1,
                    bioseq::parallelism::task_parallel_minimums::k_fragment_count,
                    bioseq::parallelism::task_parallel_region::fragment_build,
                    [&collect_range_admitted, &local_results, &window_ranges](
                            std::size_t lo, std::size_t hi) {
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            collect_range_admitted(
                                collect_range_admitted,
                                window_ranges[rel],
                                local_results[rel]);
                        }
                    });
                transfer_result_runs(local_results);
            } catch (...) {
                release_untransferred_results_noexcept(local_results);
                throw;
            }
        }

        // ---- Pass 1: k-way merge -> range-partitioned buckets + directory.
        detail::admitted_vector_run<direct_minimizer_bucket_directory_entry> admitted_dir;
        detail::admitted_vector_run<encoded_bucket_record> admitted_ob_buckets;
        detail::admitted_vector_run<std::uint64_t> admitted_ob_positions;
        std::vector<direct_minimizer_bucket_directory_entry>& dir = admitted_dir.pairs;
        std::vector<encoded_bucket_record>& ob_buckets = admitted_ob_buckets.pairs;
        std::vector<std::uint64_t>& ob_positions = admitted_ob_positions.pairs;
        std::uint64_t ob_lo = 0, ob_hi = 0;
        std::size_t ob_body_bytes = 0;                 // k_encoded_bucket_bytes*nkmers + 8*npos
        index_sub_key_t next_bucket_id = 0;
        std::uint64_t total_distinct = 0, total_positions = 0;

        auto flush_bucket = [&next_bucket_id,
                             &ob_body_bytes,
                             &storage,
                             &total_bp,
                             &ob_buckets,
                             &ob_positions,
                             &dir,
                             &ob_lo,
                             &ob_hi,
                             &admitted_dir]() {
            const index_sub_key_t sk = next_bucket_id++;
            if (sk >= k_temporary_run_sub_key_base)
                index_corruption_panic(
                    "minimizers_per_sequence: bucket id collided with reserved range");
            const std::size_t framed = k_encoded_header_bytes + ob_body_bytes;
            std::array<std::size_t,1> plan{ framed };
            storage.resize_chunks(sk, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(
                sk,
                [&framed, &total_bp, &ob_buckets, &ob_positions](
                    std::size_t c, char* outbuf, std::size_t cap) {
                    DEBUG_ASSERT(c == 0 && cap >= framed, "one bucket blob fits one chunk");
                    detail::serialize_direct_minimizer_bucket_image(
                        outbuf, total_bp, ob_buckets, ob_positions);
                    return c;
                });
            DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < ob_lo, "directory not strictly ascending (flush)");
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_dir, dir.size() + 1u)) {
                index_corruption_panic(
                    "minimizers_per_sequence: directory admission failed");
            }
            dir.push_back({ ob_lo, ob_hi, sk });
            ob_buckets.clear();
            ob_positions.clear();
            ob_body_bytes = 0;
            ob_lo = 0;
            ob_hi = 0;
        };

        storage.merge_runs([&total_distinct,
                            &total_positions,
                            &storage,
                            &next_bucket_id,
                            &flush_bucket,
                            &total_bp,
                            &admitted_ob_positions,
                            &admitted_ob_buckets,
                            &ob_positions,
                            &ob_buckets,
                            &ob_body_bytes,
                            &ob_lo,
                            &ob_hi](std::uint64_t kmer_key, auto&& positions) {
            const std::uint64_t pcount = positions.total_count();
            if (pcount > UINT32_MAX)
                index_corruption_panic("merge_runs: kmer count > u32 (encoded_bucket.pos_count)");
            ++total_distinct;
            total_positions += pcount;
            const std::size_t kmer_framed = k_encoded_header_bytes + k_encoded_bucket_bytes * 1 +
                                            sizeof(std::uint64_t) * pcount;
            if (kmer_framed > k_bucket_target_bytes) {
                // Split the heavy-hitter key across standalone direct-bucket images.
                if (!ob_buckets.empty())
                    flush_bucket();
                const index_sub_key_t sk = next_bucket_id++;
                if (sk >= k_temporary_run_sub_key_base)
                    index_corruption_panic(
                        "minimizers_per_sequence: bucket id collided with reserved range");
                detail::admitted_vector_run<std::uint64_t> admitted_kpos;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_kpos, static_cast<std::size_t>(pcount))) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy positions admission failed");
                }
                std::vector<std::uint64_t>& kpos = admitted_kpos.pairs;
                positions.for_each_batch([&kpos](std::span<const std::uint64_t> b) {
                    if (b.size()
                            > (std::numeric_limits<std::size_t>::max)()
                                - kpos.size()) {
                        index_corruption_panic(
                            "minimizers_per_sequence: heavy positions overflow");
                    }
                    kpos.insert(kpos.end(), b.begin(), b.end());
                });
                const std::size_t per = (k_bucket_target_bytes - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t);
                const std::size_t plan_count =
                    (static_cast<std::size_t>(pcount) + per - 1u) / per;
                detail::admitted_vector_run<std::size_t> admitted_plan;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_plan, plan_count)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy plan admission failed");
                }
                std::vector<std::size_t>& plan = admitted_plan.pairs;
                for (std::size_t off = 0; off < static_cast<std::size_t>(pcount); off += per) {
                    plan.push_back(
                        k_encoded_header_bytes + k_encoded_bucket_bytes +
                        sizeof(std::uint64_t) *
                            std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - off));
                }
                storage.resize_chunks(sk, std::span<const std::size_t>(plan));
                storage.template write_to_each<char>(
                    sk,
                    [&per, &pcount, &kmer_key, &total_bp, &kpos](
                        std::size_t c, char* outbuf, std::size_t) {
                        const std::size_t start = c * per;
                        const std::size_t cnt =
                            std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - start);
                        encoded_bucket_record rec{kmer_key, 0u, static_cast<std::uint32_t>(cnt)};
                        detail::serialize_direct_minimizer_bucket_image(
                            outbuf,
                            total_bp,
                            std::span<const encoded_bucket_record>(&rec, 1),
                            std::span<const std::uint64_t>(kpos.data() + start, cnt));
                        return c;
                    });
                DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < kmer_key, "directory not strictly ascending (heavy-hitter)");
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_dir, dir.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: directory admission failed");
                }
                dir.push_back({ kmer_key, kmer_key, sk });
            } else {
                const std::size_t add = k_encoded_bucket_bytes + sizeof(std::uint64_t) * pcount;
                if (k_encoded_header_bytes + ob_body_bytes + add > k_bucket_target_bytes &&
                    !ob_buckets.empty())
                    flush_bucket();
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_positions,
                        ob_positions.size() + static_cast<std::size_t>(pcount))
                    || !detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_buckets, ob_buckets.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: bucket assembly admission failed");
                }
                const std::uint32_t off = static_cast<std::uint32_t>(ob_positions.size());
                positions.for_each_batch([&ob_positions](std::span<const std::uint64_t> b) {
                    ob_positions.insert(ob_positions.end(), b.begin(), b.end());
                });
                ob_buckets.push_back(
                    encoded_bucket_record{kmer_key, off, static_cast<std::uint32_t>(pcount)});
                ob_hi = kmer_key;
                if (ob_buckets.size() == 1)
                    ob_lo = kmer_key;
                ob_body_bytes += add;
            }
        });
        if (!ob_buckets.empty())
            flush_bucket();

        // ---- write directory LAST.
        {
            const std::size_t dir_bytes =
                sizeof(direct_minimizer_bucket_directory) +
                sizeof(direct_minimizer_bucket_directory_entry) * dir.size();
            std::array<std::size_t, 1> plan{dir_bytes};
            storage.resize_chunks(k_directory_sub_key, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(
                k_directory_sub_key,
                [&dir, &total_distinct, &total_positions, &backend](
                    std::size_t c, char* outbuf, std::size_t) {
                    direct_minimizer_bucket_directory h;
                    h.magic = direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic;
                    h.layout_revision = direct_minimizer_bucket_directory::
                        k_direct_minimizer_bucket_layout_revision;
                    h.flags = backend == minimizer_sketch_backend::vulkan_segment &&
                                      !std::same_as<p_alphabet, ::seq::dna4>
                                  ? direct_minimizer_bucket_directory::k_gpu_built
                                  : 0u;
                    h.num_buckets = dir.size();
                    h.total_distinct_kmers = total_distinct;
                    h.total_positions = total_positions;
                    std::memcpy(outbuf, &h, sizeof(h));
                    if (!dir.empty())
                        std::memcpy(outbuf + sizeof(h),
                                    dir.data(),
                                    sizeof(direct_minimizer_bucket_directory_entry) * dir.size());
                    return c;
                });
        }

        // ---- Pass 2: cleanup.
        if constexpr (requires (p_storage& s) { s.snapshot_sorted_run_stats_before_release(); }) {
            storage.snapshot_sorted_run_stats_before_release();
        }
        storage.release_runs();
        if (capture_scalar_build_stats) {
            scalar_build_stats.record_completed_build();
            *scalar_build_stats_out = scalar_build_stats;
        }
    }

    template <sequence_alphabet p_alphabet>
    static void build(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        const std::uint8_t alphabet_code) {
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    // scalar-default overload delegates to the explicit backend.
    template <sequence_alphabet p_alphabet>
    static void build(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        build(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    template <sequence_alphabet p_alphabet>
    static void rebuild(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code =
            bioseq::minimizers::k_unbound_packed_alphabet_code)
    {
        if (backend == minimizer_sketch_backend::exact_simd
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_sequence rebuild"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_sequence rebuild"};
        }
        storage.clear_entity();
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    template <sequence_alphabet p_alphabet>
    static void rebuild(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        rebuild(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    /// query-time: load ONLY the buckets containing any query kmer (§10.5).
    static minimizer_hashmap_fragment_index assemble_for_query(
            p_storage& storage, std::span<const minimap_sketch_data> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            minimizer_assembly_scratch& scratch) {
        return assemble_for_query_segment(
            storage,
            query_minimizers,
            target_len,
            max_occ,
            0,
            target_len,
            scratch);
    }

    /// query-time: load query-touched buckets but keep only positions in one
    /// target collection range. Occurrence filtering remains global.
    static minimizer_hashmap_fragment_index assemble_for_query_segment(
            p_storage& storage, std::span<const minimap_sketch_data> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            minimizer_assembly_scratch& scratch) {
        using detail::k_encoded_header_bytes; using detail::k_encoded_bucket_bytes;
        scratch.clear_query();
        clip_hi_bp = std::min(clip_hi_bp, target_len);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }
        // 1. read the directory into an owned buffer (the directory view copies it).
        const char* dblob = storage.template read_chunk<char>(k_directory_sub_key, 0);
        const std::size_t dbytes = storage.chunk_byte_size(k_directory_sub_key, 0);
        storage.validate_minimizer_directory_or_error(dblob, dbytes);
        direct_minimizer_bucket_directory_view dir{ dblob, dbytes };
        const std::uint32_t max_occ_threshold =
            detail::effective_max_occ_threshold(max_occ, dir.is_gpu_built());
        // 2. collect distinct query kmers + the distinct bucket sub_keys they touch.
        auto& qkmers = scratch.query_kmers;
        auto& wanted = scratch.wanted_sub_keys;
        for (const auto& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
            if (const direct_minimizer_bucket_directory_entry* e = dir.find(qm.kmer())) wanted.push_back(e->sub_key);
        }
        auto sort_unique = [](auto& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
        sort_unique(qkmers);
        sort_unique(wanted);
        // 3. read only those buckets; deserialise; drop kmers with global count > max_occ.
        auto& pairs = scratch.pairs;
        auto emit_kept =
            [&qkmers, &max_occ_threshold, &pairs, &clip_lo_bp, &clip_hi_bp](
                std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos) {
                if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer))
                    return; // clip to QUERY kmers
                if (pos_count > max_occ_threshold)
                    return; // drop over-frequent kmer
                for (std::uint32_t j = 0; j < pos_count; ++j) {
                    const sequence_size_t pos_bp = static_cast<sequence_size_t>(gpos[j] >> 1);
                    if (pos_bp < clip_lo_bp || pos_bp >= clip_hi_bp) {
                        continue;
                    }
                    pairs.emplace_back(kmer, gpos[j]);
                }
            };
        for (std::int64_t sk : wanted) {
            const std::size_t nc = storage.number_of_chunks(sk);
            if (nc == 0) storage.index_error("minimizers_per_sequence: directory references a bucket with no chunks");
            if (nc > 1) {
                // A heavy-hitter bucket contains one k-mer; metadata gives its exact count.
                std::size_t exact = 0;
                for (std::size_t c = 0; c < nc; ++c) {
                    storage.validate_minimizer_chunk_metadata_or_error(sk, c, k_encoded_header_bytes + k_encoded_bucket_bytes, alignof(std::uint64_t));
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    exact += (cb - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t);
                }
                if (exact > static_cast<std::size_t>(max_occ_threshold)) continue;  // drop over-frequent kmer
                for (std::size_t c = 0; c < nc; ++c) {             // materialise EVERY chunk
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    const char*       p  = storage.template read_chunk<char>(sk, c);
                    const auto h = storage.validate_minimizer_blob_or_error(p, cb, 1);
                    if (h.bucket_count != 1)
                        storage.index_error("assemble_for_query: multi-chunk bucket with bucket_count!=1 (byte-math invalid)");
                    wire_for_each_bucket(p, cb, scratch.bucket_positions, emit_kept);
                }
                continue;
            }
            const std::size_t cb0 = storage.chunk_byte_size(sk, 0);
            const char*       p0  = storage.template read_chunk<char>(sk, 0);
            storage.validate_minimizer_blob_or_error(p0, cb0);
            wire_for_each_bucket(p0, cb0, scratch.bucket_positions, emit_kept);
        }
        // 4. build the single pipeline IndexT.
        minimizer_hashmap_fragment_index idx; idx.build_from_pairs(pairs, target_len);
        return idx;
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<direct_minimizer_bucket_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = k_minimizers_per_sequence_index_name;
    static constexpr bool allows_update_on_insert = true;   // opt-in external sort; not a fresh-table default
};
static_assert(requires { index_descriptor<direct_minimizer_bucket_index>::index_name; },
              "direct_minimizer_bucket_index index_descriptor must be exported");
}
