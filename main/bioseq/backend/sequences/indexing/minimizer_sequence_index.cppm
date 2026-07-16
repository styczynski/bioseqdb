/**
 * @file minimizer_sequence_index.cppm
 * @brief DOC0279 §10 — `minimizers_per_sequence`: the merged, globally-deduped,
 *        kmer-hash-range-partitioned per-sequence index. NO_POSTGRES; the spill
 *        external sort lives entirely behind the storage S.
 */
module;
#include "common.h"                       // DEBUG_ASSERT
#include <hwy/highway.h>                   // ONLY for the HWY_NAMESPACE macro
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
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;            // MinimizerHashmapFragmentIndex, build_from_pairs, wire_for_each_*
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, MinimapSketchData, MinimizerSketchBackend
import bioseq.sequences.kmers.vulkan;                // DOC0345 Patch E — collect_sorted_minimizer_pairs_vulkan_segment_or_throw
import bioseq.gpu.vulkan.phase_b_abi;                // VulkanRankLut
import bioseq.gpu.vulkan.phase_c_abi;                // Phase C sketch memory planner
import bioseq.gpu.vulkan.runtime;                    // VulkanRuntimeError
import bioseq.sequences.kmers.hashers.minimap_int;   // minimap_int_hasher
import bioseq.storage.lazy;                          // lazy_compact_sequence
import bioseq.storage.size;                          // sequence_size_t
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.common.hashing;                        // sha256_hasher / sha256_digest (§6 shared digest)
static_assert(__cplusplus >= 202302L, "C++23-or-later");

extern "C" void bioseq_sort_pair_run_u128_keys(
    hwy::uint128_t* keys,
    std::size_t key_count,
    int sort_impl) noexcept;

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers_view;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;          // DOC0345 Patch E
using bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name;   // DOC0345 Patch E — re-export so PG modules avoid the HWY_NAMESPACE macro
using bioseq::storage::sequence_size_t;
using bioseq::storage::lazy_compact_sequence;

/// Operation-originated evidence for one transient MPS build's scalar target
/// sketching.  Every collection leaf records the symbols and packed bytes it
/// actually handed to `collect_minimizers_view`; callers never infer these
/// counters from the target's advertised length.  Parallel builds aggregate
/// worker-local instances after the join, in range order.
struct ScalarMpsBuildExecutionProof {
    MinimizerSketchBackend selected_backend = MinimizerSketchBackend::Scalar;
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

    static constexpr void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void bind_backend(
            MinimizerSketchBackend backend,
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

    constexpr void record_scalar_collection(
            std::uint64_t symbols,
            std::uint64_t storage_units,
            std::uint32_t storage_unit_bytes,
            std::uint32_t letters_per_unit) noexcept {
        bind_backend(
            MinimizerSketchBackend::Scalar,
            storage_unit_bytes,
            letters_per_unit);
        bool overflow = saturated;
        add(scalar_collection_calls, 1u, overflow);
        add(input_symbols, symbols, overflow);
        add(input_storage_units, storage_units, overflow);
        std::uint64_t bytes = 0;
        if (storage_unit_bytes != 0
            && storage_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / storage_unit_bytes) {
            bytes = (std::numeric_limits<std::uint64_t>::max)();
            overflow = true;
        } else {
            bytes = storage_units * storage_unit_bytes;
        }
        add(input_bytes, bytes, overflow);
        saturated = overflow;
    }

    constexpr void combine(
            const ScalarMpsBuildExecutionProof& other) noexcept {
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

    constexpr void record_completed_build() noexcept {
        add(completed_builds, 1u, saturated);
    }

    [[nodiscard]] constexpr bool accepted(
            bool require_nonempty_work = true) const noexcept {
        if (!backend_bound || selected_backend != MinimizerSketchBackend::Scalar
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
        const ScalarMpsBuildExecutionProof&) const noexcept = default;
};

// ---- on-disk directory layout (§10.2) ------------------------------------
struct MpsV2BucketDirEntry {
    std::uint64_t lo_kmer;       // inclusive smallest kmer in the bucket
    std::uint64_t hi_kmer;       // inclusive largest kmer in the bucket
    std::int64_t  sub_key;       // bucket's storage sub_key
};
static_assert(sizeof(MpsV2BucketDirEntry) == 24 && alignof(MpsV2BucketDirEntry) == 8);

struct MpsV2BucketDirectory {
    static constexpr std::uint32_t kDirMagic   = 0xB1000002u;
    static constexpr std::uint32_t kDirVersion = 2u;
    static constexpr std::uint32_t kBuiltFromPhaseC = 1u << 0u;
    std::uint32_t magic   = kDirMagic;
    std::uint32_t version = kDirVersion;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    std::uint64_t num_buckets = 0;
    bioseq::storage::sequence_size_t total_distinct_kmers = 0;
    bioseq::storage::sequence_size_t total_positions      = 0;
    // followed by MpsV2BucketDirEntry[num_buckets]
};
static_assert(sizeof(MpsV2BucketDirectory) == 40 && alignof(MpsV2BucketDirectory) == 8,
              "directory wire header layout pinned");
static_assert(sizeof(bioseq::storage::sequence_size_t) == 8, "directory wire fields assume 8-byte sequence_size_t");
static_assert(offsetof(MpsV2BucketDirectory, flags) == 8
           && offsetof(MpsV2BucketDirectory, reserved) == 12
           && offsetof(MpsV2BucketDirectory, num_buckets) == 16
           && offsetof(MpsV2BucketDirectory, total_distinct_kmers) == 24
           && offsetof(MpsV2BucketDirectory, total_positions) == 32,
              "directory header field offsets pinned");
static_assert(offsetof(MpsV2BucketDirEntry, sub_key) == 16, "dir entry field offsets pinned");

/// Binary-search view over the directory chunk. Copies the blob into an owned
/// 8-aligned int64 vector so per-bucket read_chunk does not invalidate it.
struct MpsV2DirView {
    std::vector<std::int64_t> owned;
    const MpsV2BucketDirectory* hdr = nullptr;
    const MpsV2BucketDirEntry* entries = nullptr;
    std::size_t n = 0;
    MpsV2DirView(const char* blob, std::size_t bytes) {
        if (bytes < sizeof(MpsV2BucketDirectory)) index_corruption_panic("directory: truncated");
        owned.resize((bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t));
        std::memcpy(owned.data(), blob, bytes);
        hdr = reinterpret_cast<const MpsV2BucketDirectory*>(owned.data());
        if (hdr->magic != MpsV2BucketDirectory::kDirMagic || hdr->version != MpsV2BucketDirectory::kDirVersion)
            index_corruption_panic("directory: magic/version");
        n = static_cast<std::size_t>(hdr->num_buckets);
        const std::size_t expect = sizeof(MpsV2BucketDirectory) + sizeof(MpsV2BucketDirEntry) * n;
        if (bytes != expect) index_corruption_panic("directory: size mismatch");
        entries = reinterpret_cast<const MpsV2BucketDirEntry*>(
            reinterpret_cast<const char*>(owned.data()) + sizeof(MpsV2BucketDirectory));
        for (std::size_t i = 1; i < n; ++i)
            if (entries[i].lo_kmer <= entries[i-1].hi_kmer)
                index_corruption_panic("directory: not ascending/non-overlapping");
    }
    [[nodiscard]] const MpsV2BucketDirEntry* find(std::uint64_t kmer) const {
        std::size_t lo = 0, hi = n;
        while (lo < hi) { std::size_t mid = (lo + hi) / 2; if (entries[mid].hi_kmer < kmer) lo = mid + 1; else hi = mid; }
        if (lo < n && entries[lo].lo_kmer <= kmer && kmer <= entries[lo].hi_kmer) return &entries[lo];
        return nullptr;
    }
    [[nodiscard]] bool built_from_phase_c() const noexcept {
        return hdr != nullptr
            && (hdr->flags & MpsV2BucketDirectory::kBuiltFromPhaseC) != 0u;
    }
};

namespace detail {
using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

template <class T>
struct AdmittedVectorRun {
    // Reverse member destruction is intentional: retained vector elements and
    // capacity must disappear before the reservation becomes reusable.
    bioseq::parallelism::MemoryPermit permit;
    std::uint64_t admitted_bytes = 0;
    std::vector<T> pairs;

    AdmittedVectorRun() = default;
    AdmittedVectorRun(const AdmittedVectorRun&) = delete;
    AdmittedVectorRun& operator=(const AdmittedVectorRun&) = delete;

    AdmittedVectorRun(AdmittedVectorRun&& other) noexcept {
        // Swap while `other.permit` is still live, then transfer the permit.
        // This gives the moved-from object provably zero capacity.
        pairs.swap(other.pairs);
        admitted_bytes = std::exchange(other.admitted_bytes, 0u);
        permit = std::move(other.permit);
    }

    AdmittedVectorRun& operator=(AdmittedVectorRun&& other) noexcept {
        if (this != &other) {
            release_capacity_before_permit();
            pairs.swap(other.pairs);
            admitted_bytes = std::exchange(other.admitted_bytes, 0u);
            permit = std::move(other.permit);
        }
        return *this;
    }

    void release_capacity_before_permit() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pairs, permit);
        admitted_bytes = 0;
    }
};

using AdmittedPairRun = AdmittedVectorRun<sorted_pair_t>;

template <class T>
[[nodiscard]] std::uint64_t admitted_vector_capacity_bytes(
        const std::vector<T>& values) noexcept {
    return bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(T)));
}

template <class T>
[[nodiscard]] std::uint64_t admitted_vector_preallocation_bytes(
        std::uint64_t element_count) noexcept {
    const std::uint64_t logical_bytes = bioseq::parallelism::saturating_mul(
        element_count, static_cast<std::uint64_t>(sizeof(T)));
    return bioseq::parallelism::saturating_mul(
        logical_bytes,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
}

/**
 * Reserve an initially-empty admitted run while `transient_bytes` remains
 * covered (the Vulkan collection scratch case).  The caller must pre-acquire
 * `preallocation_headroom + transient_bytes`; after reserve we reconcile to
 * exact retained capacity plus the still-live transient budget.
 */
template <class T>
[[nodiscard]] bool reserve_admitted_vector_run_capacity(
        AdmittedVectorRun<T>& run,
        std::uint64_t element_count,
        std::uint64_t transient_bytes = 0) {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (!run.pairs.empty() || run.pairs.capacity() != 0u
        || run.admitted_bytes != 0u
        || element_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    const std::uint64_t headroom =
        admitted_vector_preallocation_bytes<T>(element_count);
    const std::uint64_t required = bioseq::parallelism::saturating_add(
        headroom, transient_bytes);
    if (headroom == max || required == max || !run.permit
        || run.permit.bytes() < required) {
        return false;
    }

    run.pairs.reserve(static_cast<std::size_t>(element_count));
    const std::uint64_t exact = admitted_vector_capacity_bytes(run.pairs);
    const std::uint64_t exact_with_transient =
        bioseq::parallelism::saturating_add(exact, transient_bytes);
    if (exact == max || exact > headroom || exact_with_transient == max
        || exact_with_transient > run.permit.bytes()) {
        return false;
    }
    run.admitted_bytes = exact;
    run.permit.shrink_to(exact_with_transient);
    return true;
}

template <class T>
[[nodiscard]] bool finish_admitted_vector_run_transient(
        AdmittedVectorRun<T>& run) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t exact = admitted_vector_capacity_bytes(run.pairs);
    if (exact == max || (exact != 0u && !run.permit)
        || exact > run.permit.bytes()) {
        return false;
    }
    run.admitted_bytes = exact;
    run.permit.shrink_to(exact);
    return true;
}

template <class T>
[[nodiscard]] bool ensure_admitted_vector_run_capacity(
        AdmittedVectorRun<T>& run,
        std::size_t required_capacity) {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t retained = admitted_vector_capacity_bytes(run.pairs);
    if (retained == max || run.admitted_bytes != retained
        || (retained != 0u
            && (!run.permit || run.permit.bytes() < retained))) {
        return false;
    }
    if (run.pairs.capacity() >= required_capacity) {
        return true;
    }
    const std::uint64_t replacement_headroom =
        admitted_vector_preallocation_bytes<T>(
            static_cast<std::uint64_t>(required_capacity));
    const std::uint64_t allocation_peak = bioseq::parallelism::saturating_add(
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
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
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
 * Reduce grossly over-reserved pair capacity without changing pair order.
 * `std::vector` cannot return capacity in place, so the old allocation plus
 * replacement headroom is admitted before constructing the replacement.  If
 * that transient peak is denied, retaining the original admitted allocation is
 * safe and semantically identical.
 */
inline void compact_admitted_pair_run_after_sort(AdmittedPairRun& run) {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t old_bytes = admitted_vector_capacity_bytes(run.pairs);
    if (old_bytes == max || (old_bytes != 0u && !run.permit)
        || run.admitted_bytes != old_bytes
        || run.permit.bytes() < old_bytes) {
        index_corruption_panic(
            "minimizers_per_sequence build: pair capacity lacks live admission");
    }
    run.permit.shrink_to(old_bytes);

    const std::uint64_t used_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(run.pairs.size()),
        static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (used_bytes == max || used_bytes > old_bytes / 2u) {
        return;
    }
    const std::uint64_t replacement_headroom =
        admitted_vector_preallocation_bytes<sorted_pair_t>(
            static_cast<std::uint64_t>(run.pairs.size()));
    const std::uint64_t allocation_peak = bioseq::parallelism::saturating_add(
        old_bytes, replacement_headroom);
    if (replacement_headroom == max || allocation_peak == max
        || !run.permit.grow_to(allocation_peak)) {
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
        if (replacement_bytes == max
            || replacement_bytes > replacement_headroom
            || bioseq::parallelism::saturating_add(
                    old_bytes, replacement_bytes) > run.permit.bytes()) {
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

constexpr std::size_t HDR = MinimizerHashmapFragmentIndex::kSerializeHeaderBytes;     // 32
constexpr std::size_t REC = MinimizerHashmapFragmentIndex::kSerializeBucketBytes;     // 16

/// Serialize a standalone v2 blob: header + bucket records + flat positions.
inline void serialize_v2_blob(char* out, sequence_size_t target_len_val,
        std::span<const BucketRecord> bkts, std::span<const std::uint64_t> poss) {
    Header h{ MinimizerHashmapFragmentIndex::kSerializeMagic,
              MinimizerHashmapFragmentIndex::kSerializeVersion,
              static_cast<std::uint64_t>(target_len_val),
              static_cast<std::uint32_t>(bkts.size()), 0u,
              static_cast<std::uint64_t>(poss.size()) };
    std::memcpy(out, &h, HDR);
    if (!bkts.empty()) std::memcpy(out + HDR, bkts.data(), REC * bkts.size());
    if (!poss.empty()) std::memcpy(out + HDR + REC * bkts.size(), poss.data(), sizeof(std::uint64_t) * poss.size());
}

enum class PairRunSortImpl : std::uint8_t {
    std_sort = 0,
    simd_u128_vqsort = 1,
    simd_u128_vec_stream_1 = 2,
    simd_u128_vec_stream_2 = 3,
    simd_u128_vec_stream_4 = 4,
};

inline int bioseq_pair_run_sort_impl = static_cast<int>(PairRunSortImpl::std_sort);

[[nodiscard]] inline PairRunSortImpl pair_run_sort_impl() noexcept {
    switch (bioseq_pair_run_sort_impl) {
    case static_cast<int>(PairRunSortImpl::simd_u128_vqsort):
        return PairRunSortImpl::simd_u128_vqsort;
    case static_cast<int>(PairRunSortImpl::simd_u128_vec_stream_1):
        return PairRunSortImpl::simd_u128_vec_stream_1;
    case static_cast<int>(PairRunSortImpl::simd_u128_vec_stream_2):
        return PairRunSortImpl::simd_u128_vec_stream_2;
    case static_cast<int>(PairRunSortImpl::simd_u128_vec_stream_4):
        return PairRunSortImpl::simd_u128_vec_stream_4;
    default:
        return PairRunSortImpl::std_sort;
    }
}

inline void set_pair_run_sort_impl(PairRunSortImpl impl) noexcept {
    bioseq_pair_run_sort_impl = static_cast<int>(impl);
}

[[nodiscard]] inline const char* pair_run_sort_impl_name() noexcept {
    switch (pair_run_sort_impl()) {
    case PairRunSortImpl::simd_u128_vqsort:
        return "simd_u128_vqsort";
    case PairRunSortImpl::simd_u128_vec_stream_1:
        return "simd_u128_vec_stream_1";
    case PairRunSortImpl::simd_u128_vec_stream_2:
        return "simd_u128_vec_stream_2";
    case PairRunSortImpl::simd_u128_vec_stream_4:
        return "simd_u128_vec_stream_4";
    case PairRunSortImpl::std_sort:
    default:
        return "std_sort";
    }
}

[[nodiscard]] constexpr std::uint32_t effective_max_occ_threshold(
    std::int32_t max_occ,
    bool built_from_phase_c) noexcept
{
    if (max_occ < 0) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }
    const std::uint32_t threshold = static_cast<std::uint32_t>(max_occ);
    if (!built_from_phase_c || threshold == 0u) {
        return threshold;
    }
    const std::uint32_t limit =
        (std::numeric_limits<std::uint32_t>::max)();
    return threshold == limit ? limit : threshold + 1u;
}

[[nodiscard]] inline hwy::uint128_t pack_pair(sorted_pair_t p) noexcept {
    return hwy::uint128_t{.lo = p.second, .hi = p.first};
}

[[nodiscard]] inline sorted_pair_t unpack_pair(hwy::uint128_t v) noexcept {
    return sorted_pair_t{v.hi, v.lo};
}

inline void sort_pair_run_u128(
        std::vector<sorted_pair_t>& pairs,
        PairRunSortImpl impl) {
    const std::uint64_t key_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(pairs.size()),
        static_cast<std::uint64_t>(sizeof(hwy::uint128_t)));
    const std::uint64_t key_preallocation_bytes =
        bioseq::parallelism::saturating_mul(
            key_bytes,
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
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
    bioseq_sort_pair_run_u128_keys(
        keys.data(),
        keys.size(),
        static_cast<int>(impl));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        pairs[i] = unpack_pair(keys[i]);
    }
}

inline void sort_pair_run(std::vector<sorted_pair_t>& pairs) {
    switch (pair_run_sort_impl()) {
    case PairRunSortImpl::simd_u128_vqsort:
        sort_pair_run_u128(pairs, PairRunSortImpl::simd_u128_vqsort);
        return;
    case PairRunSortImpl::simd_u128_vec_stream_1:
        sort_pair_run_u128(pairs, PairRunSortImpl::simd_u128_vec_stream_1);
        return;
    case PairRunSortImpl::simd_u128_vec_stream_2:
        sort_pair_run_u128(pairs, PairRunSortImpl::simd_u128_vec_stream_2);
        return;
    case PairRunSortImpl::simd_u128_vec_stream_4:
        sort_pair_run_u128(pairs, PairRunSortImpl::simd_u128_vec_stream_4);
        return;
    case PairRunSortImpl::std_sort:
    default:
        std::sort(pairs.begin(), pairs.end());
        return;
    }
}

// DOC0345 Patch E — GPU pair geometry helpers. A single GPU window's local
// positions must fit the 28-bit MinimapSketchData position field, so a Vulkan
// collect spans at most `pos_field_mask` bp of core (minus the W halo).
[[nodiscard]] inline sequence_size_t clamp_gpu_collected_bp(
    sequence_size_t wanted_bp) noexcept
{
    constexpr sequence_size_t max_bp =
        static_cast<sequence_size_t>(MinimapSketchData::pos_field_mask);
    return wanted_bp > max_bp ? max_bp : wanted_bp;
}

[[nodiscard]] inline std::size_t gpu_fragments_per_block(
    std::size_t scalar_fragments_per_block,
    std::size_t frag_bp,
    std::size_t halo_bp) noexcept
{
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(MinimapSketchData::pos_field_mask);
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

template <class Traits, std::size_t WindowSize, std::size_t KmerSize>
[[nodiscard]] inline std::uint64_t vulkan_segment_pair_collect_peak_bytes(
    std::uint64_t upload_bp) noexcept
{
    using storage_unit = typename Traits::storage_unit;
    constexpr std::uint64_t lpu =
        Traits::letters_per_unit == 0u ? 1u : Traits::letters_per_unit;
    const std::uint64_t unit_count = upload_bp / lpu + (upload_bp % lpu != 0u ? 1u : 0u);
    const std::uint64_t sequence_bytes_unaligned =
        bioseq::parallelism::saturating_mul(
            unit_count,
            static_cast<std::uint64_t>(sizeof(storage_unit)));
    const std::uint64_t sequence_bytes =
        bioseq::gpu::vulkan::phase_c_align_up(sequence_bytes_unaligned, 4u);
    const std::uint64_t max_minimizers = std::max<std::uint64_t>(64u, upload_bp);
    const bioseq::gpu::vulkan::PhaseCSketchMemoryPlan plan =
        bioseq::gpu::vulkan::phase_c_sketch_memory_plan(
            upload_bp,
            sequence_bytes,
            sizeof(bioseq::gpu::vulkan::VulkanRankLut),
            max_minimizers,
            0u,
            bioseq::gpu::vulkan::PhaseCSketchUse::sorted_pairs,
            static_cast<std::uint32_t>(KmerSize),
            static_cast<std::uint32_t>(WindowSize),
            static_cast<std::uint32_t>(lpu));
    return plan.valid
        ? plan.admission_bytes
        : (std::numeric_limits<std::uint64_t>::max)();
}
}  // namespace detail

struct MpsSegmentMergeProbe {
    template <class Positions>
    void operator()(
            std::uint32_t,
            std::uint64_t,
            Positions&& positions) const {
        (void)positions.total_count();
        positions.for_each_batch(
            [](std::span<const std::uint64_t>) noexcept {});
    }
};

template <class S>
concept MpsSegmentQualifiedSpillStorage = requires(
        S& storage,
        std::uint32_t position_segment,
        std::uint64_t span,
        std::uint64_t target_length,
        std::span<const detail::sorted_pair_t> run) {
    { storage.begin_segment_spill_generation() }
        -> std::same_as<MpsSegmentSpillStorageBinding>;
    { std::as_const(storage).segment_spill_storage_binding() }
        -> std::same_as<MpsSegmentSpillStorageBinding>;
    { storage.spill_sorted_segment_run(position_segment, run) }
        -> std::convertible_to<std::size_t>;
    { storage.merge_segment_runs(MpsSegmentMergeProbe{}) }
        -> std::same_as<void>;
    { storage.reroute_segment_runs(span, span, target_length) }
        -> std::convertible_to<bool>;
};

template <class S>
class MpsExternalSpillGenerationReplayProducer;

template <class S>
class MpsUnboundExternalSpillReplayProducer;

template <class S>
struct MinimizerSequenceMergedIndex;

// §6: the MPS content digest hashes through the single shared SHA-256 promoted
// into bioseq.common.hashing. MpsContentDigest stays as the domain-facing name
// for the 32-byte spill-evidence digest.
using MpsContentDigest = bioseq::common::sha256_digest;

/// Operation-owned proof for one persisted MPS generation. Native collection
/// is performed exactly once into PostgreSQL-owned spill runs; planning and
/// emission then replay those same runs. Format-5 performs one planning replay
/// plus separate control and position emission replays. The phases therefore
/// have deliberately separate route and replay evidence.
class MpsGenerationExecutionEvidence {
public:
    MinimizerSketchBackend selected_backend = MinimizerSketchBackend::Scalar;
    MinimizerSchemeIdentity minimizer_scheme{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    std::uint64_t configured_core_span_bp = 0;
    std::uint64_t peak_core_span_bp = 0;
    std::uint64_t peak_core_events = 0;
    std::uint64_t peak_paper_materialized_window_bp = 0;
    std::uint64_t spill_run_count = 0;
    std::uint64_t spilled_event_count = 0;
    MpsContentDigest spill_content_digest{};
    std::uint64_t spill_operation_epoch = 0;
    MpsSegmentSpillStorageBinding spill_storage_binding{};
    std::uint64_t spill_qualification_scan_count = 0;
    std::uint64_t spill_qualification_merge_count = 0;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t paper_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    ExactSimdIndexExecutionProof exact_simd_proof{};
    bool exact_simd_proof_present = false;
    PackedPaperNthashIndexStatus packed_paper_status =
        PackedPaperNthashIndexStatus::invalid_state;
    PackedPaperNthashIndexExecutionProof packed_paper_proof{};
    bool packed_paper_proof_present = false;
    bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof vulkan_proof{};
    MinimizerGenerationWireFormat generation_wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t replay_scan_count = 0;
    std::uint64_t replay_merge_pass_count = 0;
    std::uint64_t replay_reroute_pass_count = 0;
    std::uint64_t replay_completed_scan_count = 0;
    std::uint64_t replay_failed_scan_count = 0;
    std::uint64_t replay_binding_checked_failed_scan_count = 0;
    std::uint64_t replay_illegal_span_transition_count = 0;
    std::uint64_t replay_first_completed_event_count = 0;
    std::uint64_t replay_last_completed_event_count = 0;
    MpsContentDigest replay_first_completed_content_digest{};
    MpsContentDigest replay_last_completed_content_digest{};
    std::uint64_t replay_final_span_bp = 0;
    MpsSegmentSpillStorageBinding replay_final_storage_binding{};
    std::uint64_t replay_peak_callback_batch_events = 0;
    std::uint64_t replay_visited_event_count = 0;
    std::uint64_t manifest_position_count = 0;
    std::uint64_t manifest_position_segment_span_bp = 0;
    bool spill_complete = false;
    bool backend_bound = false;
    bool manifest_bound = false;
    bool saturated = false;

    [[nodiscard]] std::uint64_t
    required_successful_replay_count() const noexcept {
        if (generation_wire_format
                == MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local) {
            return 3u;
        }
        return generation_wire_format == MinimizerGenerationWireFormat::invalid
                || is_supported_minimizer_generation_wire_format(
                    generation_wire_format)
            ? 2u : 0u;
    }

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    [[nodiscard]] bool spill_accepted() const noexcept {
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
            && !exact_simd_proof_present;
        const bool no_paper = paper_nthash_core_runs == 0
            && !packed_paper_proof_present;
        const bool no_vulkan = vulkan_core_runs == 0
            && vulkan_proof.completed_roundtrips == 0
            && vulkan_proof.downloaded_minimizers == 0
            && vulkan_proof.gpu_window_splits == 0
            && !vulkan_proof.saturated;
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        const bool require_backend_work = target_length >= minimum_work_bases;
        if (!require_backend_work) {
            return scalar_core_runs == 0 && exact_simd_core_runs == 0
                && paper_nthash_core_runs == 0 && vulkan_core_runs == 0
                && no_exact && no_paper && no_vulkan
                && (target_length == 0
                    ? short_core_runs == 0
                    : short_core_runs != 0);
        }
        switch (selected_backend) {
            case MinimizerSketchBackend::Scalar:
                return scalar_core_runs != 0
                    && exact_simd_core_runs == 0
                    && paper_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_paper && no_vulkan;
            case MinimizerSketchBackend::ExactSimd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs != 0
                    && paper_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && exact_simd_proof_present
                    && exact_simd_proof.accepted(true)
                    && no_paper && no_vulkan;
            case MinimizerSketchBackend::PaperNtHashSimd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && paper_nthash_core_runs != 0
                    && vulkan_core_runs == 0
                    && no_exact && no_vulkan
                    && packed_paper_proof_present
                    && packed_paper_status
                        == PackedPaperNthashIndexStatus::ok
                    && packed_paper_proof.accepted(true);
            case MinimizerSketchBackend::VulkanSegment:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && paper_nthash_core_runs == 0
                    && vulkan_core_runs != 0
                    && no_exact && no_paper
                    && vulkan_proof.accepted(true);
        }
        return false;
    }

    [[nodiscard]] bool accepted() const noexcept {
        const std::uint64_t required_replays =
            required_successful_replay_count();
        if (!spill_accepted() || !replay_snapshot_matches_()
            || !manifest_bound
            || !manifest_authority_bound_
            || !publication_binding_present_
            || !publication_binding_.valid()
            || generation_wire_format
                != publication_binding_.wire_format
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
            && spill_storage_binding.mutation_version
                <= (std::numeric_limits<std::uint64_t>::max)()
                    - replay_reroute_pass_count
            && replay_final_storage_binding.mutation_version
                == spill_storage_binding.mutation_version
                    + replay_reroute_pass_count
            && replay_final_span_bp == manifest_position_segment_span_bp
            && replay_final_span_bp != 0;
    }

    [[nodiscard]] bool publication_accepted(
            const MinimizerGenerationPublicationBinding& expected)
            const noexcept {
        return accepted() && expected.valid()
            && publication_binding_ == expected;
    }

    /// READY may release physical spill resources only from the exact logical
    /// event-set incarnation replayed by this accepted generation.
    [[nodiscard]] bool ready_release_binding_accepted(
            MpsSegmentSpillStorageBinding before_release) const noexcept {
        return accepted() && before_release.valid()
            && before_release == replay_final_storage_binding
            && before_release.generation_token
                == spill_storage_binding.generation_token;
    }

private:
    template <class>
    friend class MpsExternalSpillGenerationReplayProducer;
    template <class>
    friend struct MinimizerSequenceMergedIndex;

    // Lifetime-safe capability copied with the evidence.  Unlike the former
    // retained MpsSpillAuthority* value, this remains meaningful after the last
    // shared authority owner is destroyed and can never dangle.
    std::uint64_t authority_seal_ = 0;
    std::uint64_t authority_event_count_ = 0;
    MpsContentDigest authority_content_digest_{};
    std::uint64_t authority_operation_epoch_ = 0;
    MpsSegmentSpillStorageBinding authority_storage_binding_{};
    bool manifest_authority_bound_ = false;
    MinimizerGenerationPublicationBinding publication_binding_{};
    bool publication_binding_present_ = false;

    struct RouteSnapshot {
        MinimizerSketchBackend selected_backend = MinimizerSketchBackend::Scalar;
        MinimizerSchemeIdentity minimizer_scheme{};
        std::uint64_t target_length = 0;
        std::uint16_t window_size = 0;
        std::uint16_t kmer_size = 0;
        std::uint64_t configured_core_span_bp = 0;
        std::uint64_t peak_core_span_bp = 0;
        std::uint64_t peak_core_events = 0;
        std::uint64_t peak_paper_materialized_window_bp = 0;
        std::uint64_t spill_run_count = 0;
        std::uint64_t spilled_event_count = 0;
        MpsContentDigest spill_content_digest{};
        std::uint64_t spill_operation_epoch = 0;
        MpsSegmentSpillStorageBinding spill_storage_binding{};
        std::uint64_t spill_qualification_scan_count = 0;
        std::uint64_t spill_qualification_merge_count = 0;
        std::uint64_t short_core_runs = 0;
        std::uint64_t scalar_core_runs = 0;
        std::uint64_t exact_simd_core_runs = 0;
        std::uint64_t paper_nthash_core_runs = 0;
        std::uint64_t vulkan_core_runs = 0;
        ExactSimdIndexExecutionProof exact_simd_proof{};
        bool exact_simd_proof_present = false;
        PackedPaperNthashIndexStatus packed_paper_status =
            PackedPaperNthashIndexStatus::invalid_state;
        PackedPaperNthashIndexExecutionProof packed_paper_proof{};
        bool packed_paper_proof_present = false;
        bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof vulkan_proof{};
        bool spill_complete = false;
        bool backend_bound = false;
        bool saturated = false;
    } route_snapshot_{};
    bool route_snapshot_present_ = false;

    struct ReplaySnapshot {
        MinimizerGenerationWireFormat generation_wire_format =
            MinimizerGenerationWireFormat::invalid;
        std::uint64_t replay_scan_count = 0;
        std::uint64_t replay_merge_pass_count = 0;
        std::uint64_t replay_reroute_pass_count = 0;
        std::uint64_t replay_completed_scan_count = 0;
        std::uint64_t replay_failed_scan_count = 0;
        std::uint64_t replay_binding_checked_failed_scan_count = 0;
        std::uint64_t replay_illegal_span_transition_count = 0;
        std::uint64_t replay_first_completed_event_count = 0;
        std::uint64_t replay_last_completed_event_count = 0;
        MpsContentDigest replay_first_completed_content_digest{};
        MpsContentDigest replay_last_completed_content_digest{};
        std::uint64_t replay_final_span_bp = 0;
        MpsSegmentSpillStorageBinding replay_final_storage_binding{};
        std::uint64_t replay_peak_callback_batch_events = 0;
        std::uint64_t replay_visited_event_count = 0;
        std::uint64_t manifest_position_count = 0;
        std::uint64_t manifest_position_segment_span_bp = 0;
        bool manifest_bound = false;
        MinimizerGenerationPublicationBinding publication_binding{};
        bool publication_binding_present = false;
    } replay_snapshot_{};
    bool replay_snapshot_present_ = false;

    [[nodiscard]] static bool same_vulkan_proof_(
            const bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof& lhs,
            const bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof& rhs)
            noexcept {
        return lhs.completed_roundtrips == rhs.completed_roundtrips
            && lhs.downloaded_minimizers == rhs.downloaded_minimizers
            && lhs.gpu_window_splits == rhs.gpu_window_splits
            && lhs.saturated == rhs.saturated;
    }

    void capture_route_snapshot_() noexcept {
        route_snapshot_ = RouteSnapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
            .target_length = target_length,
            .window_size = window_size,
            .kmer_size = kmer_size,
            .configured_core_span_bp = configured_core_span_bp,
            .peak_core_span_bp = peak_core_span_bp,
            .peak_core_events = peak_core_events,
            .peak_paper_materialized_window_bp =
                peak_paper_materialized_window_bp,
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
            .paper_nthash_core_runs = paper_nthash_core_runs,
            .vulkan_core_runs = vulkan_core_runs,
            .exact_simd_proof = exact_simd_proof,
            .exact_simd_proof_present = exact_simd_proof_present,
            .packed_paper_status = packed_paper_status,
            .packed_paper_proof = packed_paper_proof,
            .packed_paper_proof_present = packed_paper_proof_present,
            .vulkan_proof = vulkan_proof,
            .spill_complete = spill_complete,
            .backend_bound = backend_bound,
            .saturated = saturated,
        };
        route_snapshot_present_ = true;
    }

    [[nodiscard]] bool route_snapshot_matches_() const noexcept {
        const RouteSnapshot& value = route_snapshot_;
        return route_snapshot_present_
            && selected_backend == value.selected_backend
            && minimizer_scheme == value.minimizer_scheme
            && target_length == value.target_length
            && window_size == value.window_size
            && kmer_size == value.kmer_size
            && configured_core_span_bp == value.configured_core_span_bp
            && peak_core_span_bp == value.peak_core_span_bp
            && peak_core_events == value.peak_core_events
            && peak_paper_materialized_window_bp
                == value.peak_paper_materialized_window_bp
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
            && paper_nthash_core_runs == value.paper_nthash_core_runs
            && vulkan_core_runs == value.vulkan_core_runs
            && exact_simd_proof == value.exact_simd_proof
            && exact_simd_proof_present == value.exact_simd_proof_present
            && packed_paper_status == value.packed_paper_status
            && packed_paper_proof == value.packed_paper_proof
            && packed_paper_proof_present == value.packed_paper_proof_present
            && same_vulkan_proof_(vulkan_proof, value.vulkan_proof)
            && spill_complete == value.spill_complete
            && backend_bound == value.backend_bound
            && saturated == value.saturated;
    }

    void capture_replay_snapshot_() noexcept {
        replay_snapshot_ = ReplaySnapshot{
            .generation_wire_format = generation_wire_format,
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
        const ReplaySnapshot& value = replay_snapshot_;
        return replay_snapshot_present_
            && generation_wire_format == value.generation_wire_format
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
            const MpsContentDigest& content_digest,
            MpsSegmentSpillStorageBinding storage_binding,
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
            const MpsContentDigest& content_digest,
            MpsSegmentSpillStorageBinding storage_binding,
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
            const EncodedMinimizerGeneration& generation) noexcept {
        publication_binding_ =
            minimizer_generation_publication_binding(generation);
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
template <class S>
class MpsSpillAuthority {
private:
    template <class>
    friend struct MinimizerSequenceMergedIndex;
    template <class>
    friend class MpsExternalSpillGenerationReplayProducer;

    S* storage_ = nullptr;
    std::uint64_t target_length_ = 0;
    std::uint64_t initial_span_bp_ = 0;
    std::uint64_t run_count_ = 0;
    std::uint64_t event_count_ = 0;
    MpsContentDigest content_digest_{};
    MpsSegmentSpillStorageBinding storage_binding_{};
    std::uint64_t operation_epoch_ = 0;
    std::uint64_t authority_seal_ = 0;
    MinimizerSchemeIdentity minimizer_scheme_{};

    MpsSpillAuthority(
            S& storage,
            std::uint64_t target_length,
            std::uint64_t initial_span_bp,
            std::uint64_t run_count,
            std::uint64_t event_count,
            const MpsContentDigest& content_digest,
            MpsSegmentSpillStorageBinding storage_binding,
            std::uint64_t operation_epoch,
            std::uint64_t authority_seal,
            MinimizerSchemeIdentity minimizer_scheme) noexcept
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
    MpsSpillAuthority(const MpsSpillAuthority&) = delete;
    MpsSpillAuthority& operator=(const MpsSpillAuthority&) = delete;
    ~MpsSpillAuthority() = default;
};

/// Concrete MPS revision-3 producer over the existing PostgreSQL-owned
/// external spill store. The store's bounded k-way merge owns its cursors and
/// batches; this adapter owns no event or run payload. Replaying once per
/// persisted core supplies the common encoder's segment-major canonical order.
template <class S>
class MpsExternalSpillGenerationReplayProducer {
private:
    S* storage_ = nullptr;
    MinimizerSchemeIdentity minimizer_scheme_{};
    std::uint64_t target_length_ = 0;
    std::uint64_t qualified_span_ = 0;
    MinimizerGenerationWireFormat generation_wire_format_ =
        MinimizerGenerationWireFormat::invalid;
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
    MpsContentDigest first_completed_content_digest_{};
    MpsContentDigest last_completed_content_digest_{};
    std::uint64_t final_span_bp_ = 0;
    MpsSegmentSpillStorageBinding storage_binding_{};
    MpsContentDigest expected_replay_digest_{};
    bool expected_replay_digest_valid_ = false;
    std::uint64_t binding_checked_failed_scan_count_ = 0;
    bool has_completed_scan_ = false;
    bool saturated_ = false;
    bool terminal_ = false;
    bool requires_halving_after_failed_scan_ = false;
    MpsGenerationExecutionEvidence spill_evidence_{};
    std::shared_ptr<const MpsSpillAuthority<S>> authority_{};

    void add_counter_(
            std::uint64_t& destination,
            std::uint64_t increment = 1u) noexcept {
        MpsGenerationExecutionEvidence::add(
            destination, increment, saturated_);
    }

    [[nodiscard]] std::uint64_t
    required_successful_replay_count_() const noexcept {
        if (generation_wire_format_
                == MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local) {
            return 3u;
        }
        return generation_wire_format_
                    == MinimizerGenerationWireFormat::invalid
                || is_supported_minimizer_generation_wire_format(
                    generation_wire_format_)
            ? 2u : 0u;
    }

    void record_completed_scan_(
            std::uint64_t event_count,
            const MpsContentDigest& content_digest,
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

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    fail_scan_(MinimizerGenerationWireError error) noexcept {
        add_counter_(failed_scan_count_);
        return std::unexpected(error);
    }

    template <class>
    friend struct MinimizerSequenceMergedIndex;
    template <class>
    friend class MpsUnboundExternalSpillReplayProducer;

    MpsExternalSpillGenerationReplayProducer(
            S& storage,
            std::uint64_t target_length,
            std::uint64_t qualified_span,
            MinimizerSchemeIdentity minimizer_scheme,
            const MpsGenerationExecutionEvidence& spill_evidence = {},
            std::shared_ptr<const MpsSpillAuthority<S>> authority = {},
            MpsSegmentSpillStorageBinding storage_binding = {}) noexcept
        : storage_(&storage),
          minimizer_scheme_(minimizer_scheme),
          target_length_(target_length),
          qualified_span_(qualified_span),
          spill_evidence_(spill_evidence),
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
    MpsExternalSpillGenerationReplayProducer(
        const MpsExternalSpillGenerationReplayProducer&) = delete;
    MpsExternalSpillGenerationReplayProducer& operator=(
        const MpsExternalSpillGenerationReplayProducer&) = delete;
    MpsExternalSpillGenerationReplayProducer(
        MpsExternalSpillGenerationReplayProducer&&) = delete;
    MpsExternalSpillGenerationReplayProducer& operator=(
        MpsExternalSpillGenerationReplayProducer&&) = delete;

    void bind_generation_wire_format(
            MinimizerGenerationWireFormat wire_format) noexcept {
        if (!is_supported_minimizer_generation_wire_format(wire_format)
            || scan_count_ != 0u
            || (generation_wire_format_
                    != MinimizerGenerationWireFormat::invalid
                && generation_wire_format_ != wire_format)) {
            saturated_ = true;
            terminal_ = true;
            return;
        }
        generation_wire_format_ = wire_format;
    }

    [[nodiscard]] MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return minimizer_scheme_;
    }

    /// The persisted physical producer identity comes only from the sealed
    /// native spill evidence. A raw/unbound replay cannot label its historical
    /// event stream as Scalar, SIMD, or Vulkan merely from shared scheme bits.
    [[nodiscard]] MinimizerProducerBackendIdentity
    producer_backend_identity() const noexcept {
        if (!spill_evidence_.spill_accepted()) {
            return {};
        }
        const MinimizerProducerBackendIdentity identity =
            minimizer_producer_backend_identity_for_backend(
                spill_evidence_.selected_backend);
        return minimizer_scheme_identity_for_producer_backend(identity)
                == minimizer_scheme_
            ? identity : MinimizerProducerBackendIdentity{};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t persisted_span,
            CanonicalMinimizerEventReceiver receiver) {
        add_counter_(scan_count_);
        if (terminal_ || storage_ == nullptr || persisted_span == 0) {
            return fail_scan_(MinimizerGenerationWireError::segment_span);
        }
        if (!storage_binding_.valid()
            || storage_->segment_spill_storage_binding()
                != storage_binding_) {
            terminal_ = true;
            return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
        }
        if (target_length_ == 0) {
            bioseq::common::sha256_hasher empty_hasher;
            const MpsContentDigest empty_digest = empty_hasher.finalize();
            if (storage_->segment_spill_storage_binding()
                    != storage_binding_) {
                terminal_ = true;
                return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
            }
            if (authority_ != nullptr
                && (authority_->storage_ != storage_
                    || authority_->target_length_ != target_length_
                    || authority_->event_count_ != 0
                    || authority_->content_digest_ != empty_digest
                    || authority_->operation_epoch_ == 0
                    || authority_->storage_binding_ != storage_binding_
                    || authority_->minimizer_scheme_ != minimizer_scheme_
                    || !spill_evidence_.authority_matches(
                        authority_->authority_seal_,
                        0,
                        empty_digest,
                        authority_->storage_binding_,
                        authority_->operation_epoch_))) {
                terminal_ = true;
                return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
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
                return fail_scan_(MinimizerGenerationWireError::segment_span);
            }
            const MpsSegmentSpillStorageBinding before_reroute =
                storage_binding_;
            if (reroute_pass_count_
                    == (std::numeric_limits<std::uint64_t>::max)()
                || !storage_->reroute_segment_runs(
                    qualified_span_, persisted_span, target_length_)) {
                terminal_ = true;
                return fail_scan_(MinimizerGenerationWireError::position_range);
            }
            const MpsSegmentSpillStorageBinding after_reroute =
                storage_->segment_spill_storage_binding();
            if (!after_reroute.valid()
                || after_reroute.generation_token
                    != before_reroute.generation_token
                || before_reroute.mutation_version
                    == (std::numeric_limits<std::uint64_t>::max)()
                || after_reroute.mutation_version
                    != before_reroute.mutation_version + 1u) {
                terminal_ = true;
                return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
            }
            add_counter_(reroute_pass_count_);
            qualified_span_ = persisted_span;
            storage_binding_ = after_reroute;
            expected_replay_digest_valid_ = false;
            requires_halving_after_failed_scan_ = false;
        } else if (requires_halving_after_failed_scan_) {
            add_counter_(illegal_span_transition_count_);
            terminal_ = true;
            return fail_scan_(MinimizerGenerationWireError::segment_span);
        }
        const std::uint64_t segment_count = target_length_ / persisted_span
            + (target_length_ % persisted_span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            terminal_ = true;
            return fail_scan_(MinimizerGenerationWireError::segment_count);
        }
        if (merge_pass_count_
                == (std::numeric_limits<std::uint64_t>::max)()) {
            terminal_ = true;
            return fail_scan_(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        add_counter_(merge_pass_count_);
        std::optional<MinimizerGenerationWireError> structural_error;
        std::optional<MinimizerGenerationWireError> receiver_error;
        std::uint64_t scan_event_count = 0;
        bioseq::common::sha256_hasher scan_content_hasher;
        std::uint32_t previous_segment = 0;
        bool have_segment = false;
        storage_->merge_segment_runs(
            [&](std::uint32_t segment,
                std::uint64_t hash,
                auto&& positions) {
                if (structural_error) {
                    return;
                }
                if (segment >= segment_count
                    || (have_segment && segment < previous_segment)) {
                    structural_error =
                        MinimizerGenerationWireError::segment_ordinal;
                    return;
                }
                have_segment = true;
                previous_segment = segment;
                positions.for_each_batch(
                    [&](std::span<const std::uint64_t> batch) {
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
                                    MinimizerGenerationWireError::position_range;
                                return;
                            }
                            if (visited_event_count_
                                    == (std::numeric_limits<std::uint64_t>::max)()) {
                                structural_error =
                                    MinimizerGenerationWireError::arithmetic_overflow;
                                return;
                            }
                            add_counter_(visited_event_count_);
                            add_counter_(scan_event_count);
                            scan_content_hasher.update_little_endian(hash);
                            scan_content_hasher.update_little_endian(
                                encoded_position);
                            if (!receiver_error) {
                                if (auto accepted = receiver(
                                    CanonicalMinimizerEvent{
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
            structural_error = MinimizerGenerationWireError::replay_mismatch;
        }
        if (structural_error) {
            terminal_ = true;
            return fail_scan_(*structural_error);
        }
        const MpsContentDigest content_digest =
            scan_content_hasher.finalize();
        if (authority_ != nullptr
            && (authority_->storage_ != storage_
                || authority_->target_length_ != target_length_
                || authority_->event_count_ != scan_event_count
                || authority_->operation_epoch_ == 0
                || authority_->minimizer_scheme_ != minimizer_scheme_
                || !spill_evidence_.authority_matches(
                    authority_->authority_seal_,
                    authority_->event_count_,
                    authority_->content_digest_,
                    authority_->storage_binding_,
                    authority_->operation_epoch_))) {
            terminal_ = true;
            return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
        }
        if (expected_replay_digest_valid_) {
            if (content_digest != expected_replay_digest_) {
                terminal_ = true;
                return fail_scan_(MinimizerGenerationWireError::replay_mismatch);
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
    [[nodiscard]] MpsGenerationExecutionEvidence
    execution_evidence() const noexcept {
        MpsGenerationExecutionEvidence evidence = spill_evidence_;
        evidence.generation_wire_format = generation_wire_format_;
        evidence.replay_scan_count = scan_count_;
        evidence.replay_merge_pass_count = merge_pass_count_;
        evidence.replay_reroute_pass_count = reroute_pass_count_;
        evidence.replay_completed_scan_count = completed_scan_count_;
        evidence.replay_failed_scan_count = failed_scan_count_;
        evidence.replay_binding_checked_failed_scan_count =
            binding_checked_failed_scan_count_;
        evidence.replay_illegal_span_transition_count =
            illegal_span_transition_count_;
        evidence.replay_first_completed_event_count =
            first_completed_event_count_;
        evidence.replay_last_completed_event_count =
            last_completed_event_count_;
        evidence.replay_first_completed_content_digest =
            first_completed_content_digest_;
        evidence.replay_last_completed_content_digest =
            last_completed_content_digest_;
        evidence.replay_final_span_bp = final_span_bp_;
        evidence.replay_final_storage_binding = storage_binding_;
        evidence.replay_peak_callback_batch_events =
            peak_callback_batch_events_;
        evidence.replay_visited_event_count = visited_event_count_;
        evidence.saturated = evidence.saturated || saturated_;
        return evidence;
    }
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

/// Explicitly unbound replay seam for geometry/canonical-order tests. It can
/// exercise retries but never carries a native route snapshot or spill
/// authority, so its evidence cannot satisfy `accepted()`.
template <class S>
class MpsUnboundExternalSpillReplayProducer {
private:
    static MpsSegmentSpillStorageBinding binding_for_(S& storage) noexcept {
        MpsSegmentSpillStorageBinding binding =
            storage.segment_spill_storage_binding();
        if (!binding.valid()) {
            binding = storage.begin_segment_spill_generation();
        }
        return binding;
    }

    MpsExternalSpillGenerationReplayProducer<S> producer_;

public:
    MpsUnboundExternalSpillReplayProducer(
            S& storage,
            std::uint64_t target_length,
            std::uint64_t qualified_span,
            MinimizerSchemeIdentity minimizer_scheme) noexcept
        : producer_(
            storage,
            target_length,
            qualified_span,
            minimizer_scheme,
            {},
            {},
            binding_for_(storage)) {}

    MpsUnboundExternalSpillReplayProducer(
        const MpsUnboundExternalSpillReplayProducer&) = delete;
    MpsUnboundExternalSpillReplayProducer& operator=(
        const MpsUnboundExternalSpillReplayProducer&) = delete;
    MpsUnboundExternalSpillReplayProducer(
        MpsUnboundExternalSpillReplayProducer&&) = delete;
    MpsUnboundExternalSpillReplayProducer& operator=(
        MpsUnboundExternalSpillReplayProducer&&) = delete;

    void bind_generation_wire_format(
            MinimizerGenerationWireFormat wire_format) noexcept {
        producer_.bind_generation_wire_format(wire_format);
    }

    [[nodiscard]] MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return producer_.minimizer_scheme_identity();
    }
    [[nodiscard]] MinimizerProducerBackendIdentity
    producer_backend_identity() const noexcept {
        return producer_.producer_backend_identity();
    }
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t persisted_span,
            CanonicalMinimizerEventReceiver receiver) {
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
    [[nodiscard]] MpsGenerationExecutionEvidence
    execution_evidence() const noexcept {
        return producer_.execution_evidence();
    }
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

template <class S>
struct MinimizerSequenceMergedIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
    static_assert(is_external_sort_spill_storage<S>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
    using storage_type = S;

    class GenerationSpillResult {
    private:
        friend struct MinimizerSequenceMergedIndex<S>;

        std::uint64_t run_count_ = 0;
        ExactSimdIndexExecutionProof exact_simd_proof_{};
        bool exact_simd_proof_present_ = false;
        MinimizerSchemeIdentity minimizer_scheme_{};
        std::uint64_t target_length_ = 0;
        std::uint64_t position_segment_span_bp_ = 0;
        MpsGenerationExecutionEvidence execution_evidence_{};
        std::shared_ptr<const MpsSpillAuthority<S>> authority_{};

        GenerationSpillResult(
                std::uint64_t run_count,
                const ExactSimdIndexExecutionProof& exact_simd_proof,
                bool exact_simd_proof_present,
                MinimizerSchemeIdentity minimizer_scheme,
                std::uint64_t target_length,
                std::uint64_t position_segment_span_bp,
                const MpsGenerationExecutionEvidence& execution_evidence,
                std::shared_ptr<const MpsSpillAuthority<S>> authority) noexcept
            : run_count_(run_count),
              exact_simd_proof_(exact_simd_proof),
              exact_simd_proof_present_(exact_simd_proof_present),
              minimizer_scheme_(minimizer_scheme),
              target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_evidence_(execution_evidence),
              authority_(std::move(authority)) {}

        void poison_() noexcept {
            run_count_ = 0;
            exact_simd_proof_ = {};
            exact_simd_proof_present_ = false;
            minimizer_scheme_ = {};
            target_length_ = 0;
            position_segment_span_bp_ = 0;
            execution_evidence_ = {};
            authority_.reset();
        }

        void steal_from_(GenerationSpillResult&& other) noexcept {
            run_count_ = other.run_count_;
            exact_simd_proof_ = other.exact_simd_proof_;
            exact_simd_proof_present_ = other.exact_simd_proof_present_;
            minimizer_scheme_ = other.minimizer_scheme_;
            target_length_ = other.target_length_;
            position_segment_span_bp_ = other.position_segment_span_bp_;
            execution_evidence_ = std::move(other.execution_evidence_);
            authority_ = std::move(other.authority_);
            other.poison_();
        }

    public:
        GenerationSpillResult() = delete;
        GenerationSpillResult(const GenerationSpillResult&) = delete;
        GenerationSpillResult& operator=(const GenerationSpillResult&) = delete;
        GenerationSpillResult(GenerationSpillResult&& other) noexcept {
            steal_from_(std::move(other));
        }
        GenerationSpillResult& operator=(GenerationSpillResult&& other) noexcept {
            if (this != &other) {
                poison_();
                steal_from_(std::move(other));
            }
            return *this;
        }

        [[nodiscard]] std::uint64_t run_count() const noexcept {
            return run_count_;
        }
        [[nodiscard]] const ExactSimdIndexExecutionProof&
        exact_simd_proof() const noexcept {
            return exact_simd_proof_;
        }
        [[nodiscard]] bool exact_simd_proof_present() const noexcept {
            return exact_simd_proof_present_;
        }
        [[nodiscard]] MinimizerSchemeIdentity
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
        [[nodiscard]] const MpsGenerationExecutionEvidence&
        execution_evidence() const noexcept {
            return execution_evidence_;
        }
    };

    template <class R, class Scanner>
    [[nodiscard]] static std::expected<
        SegmentUnitRange,
        MinimizerGenerationWireError>
    make_generation_collection_range(
            lazy_compact_sequence<R, Scanner>& sequence,
            std::uint64_t core_lo,
            std::uint64_t core_hi,
            std::uint64_t halo_bp = 10u + 15u - 2u) noexcept {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::uint64_t lpu = traits::letters_per_unit;
        static_assert(
            sizeof(typename traits::storage_unit)
                <= (std::numeric_limits<std::uint32_t>::max)());
        static_assert(
            lpu <= (std::numeric_limits<std::uint32_t>::max)());
        const std::uint64_t target_length = sequence.bp_size();
        if (core_lo > core_hi || core_hi > target_length || lpu == 0) {
            return std::unexpected(MinimizerGenerationWireError::bp_range);
        }
        const std::uint64_t collection_lo = core_lo >= halo_bp
            ? core_lo - halo_bp : 0u;
        const std::uint64_t collection_hi = core_hi
                > target_length - std::min(target_length, halo_bp)
            ? target_length : core_hi + halo_bp;
        const std::uint64_t cs_u = collection_lo / lpu;
        const std::uint64_t ce_u = collection_hi / lpu
            + (collection_hi % lpu != 0 ? 1u : 0u);
        if (cs_u > (std::numeric_limits<sequence_size_t>::max)()
            || ce_u > (std::numeric_limits<sequence_size_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        return SegmentUnitRange{
            .cs_u = static_cast<sequence_size_t>(cs_u),
            .ce_u = static_cast<sequence_size_t>(ce_u),
            .core_lo_bp = static_cast<sequence_size_t>(core_lo),
            .core_hi_bp = static_cast<sequence_size_t>(core_hi),
        };
    }

    template <class R, class Scanner>
    [[nodiscard]] static std::expected<
        ExactSimdIndexExecutionProof,
        MinimizerGenerationWireError>
    collect_generation_core_run(
            lazy_compact_sequence<R, Scanner>& sequence,
            MinimizerSketchBackend backend,
            SegmentUnitRange range,
            sketch_collector& collector,
            std::vector<detail::sorted_pair_t>& output,
            MpsGenerationExecutionEvidence* execution_evidence = nullptr,
            bool allow_packed_paper = false,
            ScalarMpsBuildExecutionProof* scalar_build_proof = nullptr) {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::uint64_t lpu = traits::letters_per_unit;
        const std::uint64_t target_length = sequence.bp_size();
        const std::uint64_t size_units = target_length / lpu
            + (target_length % lpu != 0 ? 1u : 0u);
        if (range.core_lo_bp > range.core_hi_bp
            || range.core_hi_bp > target_length
            || range.cs_u > range.ce_u || range.ce_u > size_units) {
            return std::unexpected(MinimizerGenerationWireError::bp_range);
        }
        if (range.core_lo_bp == range.core_hi_bp
            || range.cs_u == range.ce_u) {
            return ExactSimdIndexExecutionProof{};
        }
        if (range.cs_u
                > (std::numeric_limits<std::uint64_t>::max)() / lpu) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        const std::uint64_t emit_base = range.cs_u * lpu;
        constexpr std::uint64_t halo_bp = 10u + 15u - 2u;
        const std::uint64_t collection_hi = range.core_hi_bp
                > target_length - std::min(target_length, halo_bp)
            ? target_length : range.core_hi_bp + halo_bp;
        const std::uint64_t local_bp_size = collection_hi > emit_base
            ? collection_hi - emit_base : 0u;
        constexpr std::uint64_t minimum_work_bases = 10u + 15u - 1u;
        const auto add_route = [execution_evidence](
                std::uint64_t MpsGenerationExecutionEvidence::* member,
                std::uint64_t increment = 1u) noexcept {
            if (execution_evidence != nullptr) {
                MpsGenerationExecutionEvidence::add(
                    execution_evidence->*member,
                    increment,
                    execution_evidence->saturated);
            }
        };
        if (local_bp_size < minimum_work_bases) {
            add_route(&MpsGenerationExecutionEvidence::short_core_runs);
            return ExactSimdIndexExecutionProof{};
        }
        if (backend == MinimizerSketchBackend::VulkanSegment) {
            add_route(&MpsGenerationExecutionEvidence::vulkan_core_runs);
            bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof core_proof{};
            auto pairs =
                bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    sequence,
                    static_cast<std::size_t>(range.cs_u),
                    static_cast<std::size_t>(range.ce_u),
                    emit_base,
                    range.core_lo_bp,
                    range.core_hi_bp,
                    10,
                    15,
                    execution_evidence != nullptr ? &core_proof : nullptr);
            if (execution_evidence != nullptr) {
                execution_evidence->vulkan_proof.combine(core_proof);
                execution_evidence->saturated =
                    execution_evidence->saturated
                    || execution_evidence->vulkan_proof.saturated;
            }
            output.insert(
                output.end(),
                std::make_move_iterator(pairs.begin()),
                std::make_move_iterator(pairs.end()));
            return ExactSimdIndexExecutionProof{};
        }
        if (backend != MinimizerSketchBackend::Scalar
            && backend != MinimizerSketchBackend::ExactSimd
            && backend != MinimizerSketchBackend::PaperNtHashSimd) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }

        collector.clear();
        ExactSimdIndexExecutionProof exact_simd_proof{};
        if (backend == MinimizerSketchBackend::ExactSimd) {
            add_route(&MpsGenerationExecutionEvidence::exact_simd_core_runs);
            exact_simd_proof = collect_exact_simd_index_range_or_throw(
                    collector,
                    sequence,
                    static_cast<std::size_t>(range.cs_u),
                    static_cast<std::size_t>(range.ce_u),
                    10,
                    15,
                    0,
                    "minimizers_per_sequence build");
            if (execution_evidence != nullptr) {
                execution_evidence->exact_simd_proof.combine(
                    exact_simd_proof);
                execution_evidence->exact_simd_proof_present = true;
                execution_evidence->saturated =
                    execution_evidence->saturated
                    || execution_evidence->exact_simd_proof.saturated;
            }
        } else if (backend == MinimizerSketchBackend::PaperNtHashSimd) {
            if (!allow_packed_paper) {
                return std::unexpected(
                    MinimizerGenerationWireError::compatibility_mismatch);
            }
            add_route(&MpsGenerationExecutionEvidence::paper_nthash_core_runs);
            std::uint64_t materialized_window_bp = 0;
            const auto paper_result =
                collect_bounded_packed_paper_nthash_index_lazy(
                    sequence,
                    range.cs_u,
                    range.ce_u,
                    local_bp_size,
                    10,
                    15,
                    collector,
                    &materialized_window_bp);
            if (execution_evidence != nullptr) {
                execution_evidence->peak_paper_materialized_window_bp =
                    std::max(
                        execution_evidence->peak_paper_materialized_window_bp,
                        materialized_window_bp);
                execution_evidence->packed_paper_status = paper_result.status;
                execution_evidence->packed_paper_proof.combine(
                    paper_result.proof);
                execution_evidence->packed_paper_proof_present = true;
                execution_evidence->saturated =
                    execution_evidence->saturated
                    || execution_evidence->packed_paper_proof.saturated;
            }
            if (paper_result.status != PackedPaperNthashIndexStatus::ok) {
                return std::unexpected(
                    MinimizerGenerationWireError::compatibility_mismatch);
            }
        } else {
            add_route(&MpsGenerationExecutionEvidence::scalar_core_runs);
            auto begin = sequence.cbegin();
            begin += range.cs_u;
            auto end = sequence.cbegin();
            end += range.ce_u;
            collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
                collector, sequence, begin, end, 10, 15);
            if (scalar_build_proof != nullptr) {
                constexpr std::uint64_t maximum =
                    (std::numeric_limits<std::uint64_t>::max)();
                const std::uint64_t nominal_end = range.ce_u > maximum / lpu
                    ? maximum
                    : static_cast<std::uint64_t>(range.ce_u) * lpu;
                const std::uint64_t actual_begin =
                    std::min(target_length, emit_base);
                const std::uint64_t actual_end =
                    std::min(target_length, nominal_end);
                scalar_build_proof->record_scalar_collection(
                    actual_end >= actual_begin
                        ? actual_end - actual_begin : 0u,
                    static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                    static_cast<std::uint32_t>(
                        sizeof(typename traits::storage_unit)),
                    static_cast<std::uint32_t>(lpu));
            }
        }
        for (const MinimapSketchData& minimizer
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
        return exact_simd_proof;
    }

    /// Bounded serial preparation seam for rev3 POC/tests. Each source core is
    /// collected and spilled before the next is allocated; the returned MPS
    /// producer reopens these same external runs for both encoder passes.
    template <class R, class Scanner>
    [[nodiscard]] static std::expected<
        GenerationSpillResult,
        MinimizerGenerationWireError>
    spill_generation_replay_runs_serial(
            S& storage,
            lazy_compact_sequence<R, Scanner>& sequence,
            MinimizerSketchBackend backend,
            std::uint64_t core_span_bp = 0,
            std::uint64_t position_segment_span_bp =
                kMinimizerPreferredSegmentSpanBp,
            MpsGenerationExecutionEvidence* execution_evidence_out = nullptr) {
        static_assert(MpsSegmentQualifiedSpillStorage<S>);
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::uint64_t lpu = traits::letters_per_unit;
        static_assert(lpu > 0);
        static_assert(Scanner::fragment_storage_units
            <= (std::numeric_limits<std::uint64_t>::max)() / lpu);
        constexpr std::uint64_t default_core_span =
            static_cast<std::uint64_t>(Scanner::fragment_storage_units) * lpu;
        const std::uint64_t target_length = sequence.bp_size();
        struct EvidenceOutputGuard {
            MpsGenerationExecutionEvidence* output = nullptr;
            MpsGenerationExecutionEvidence evidence{};
            ~EvidenceOutputGuard() {
                if (output != nullptr) {
                    *output = evidence;
                }
            }
        } evidence_guard;
        evidence_guard.output = execution_evidence_out;
        MpsGenerationExecutionEvidence& evidence = evidence_guard.evidence;
        evidence.selected_backend = backend;
        evidence.minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        evidence.target_length = target_length;
        evidence.window_size = 10;
        evidence.kmer_size = 15;
        evidence.configured_core_span_bp =
            core_span_bp == 0 ? default_core_span : core_span_bp;
        if (core_span_bp == 0) {
            core_span_bp = default_core_span;
        }
        if (core_span_bp == 0 || core_span_bp > default_core_span
            || position_segment_span_bp == 0) {
            return std::unexpected(MinimizerGenerationWireError::segment_span);
        }
        const MinimizerSchemeIdentity minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        if (!is_supported_minimizer_scheme_identity(
                minimizer_scheme.id, minimizer_scheme.version)) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        MpsSegmentSpillStorageBinding storage_binding =
            storage.begin_segment_spill_generation();
        if (!storage_binding.valid()) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        evidence.backend_bound = true;
        std::uint64_t segment_lo = 0;
        std::uint64_t segment_ordinal = 0;
        {
        struct ReplayBuffers {
            // Reverse destruction drops both vector owners before admission.
            bioseq::parallelism::MemoryPermit permit;
            std::vector<detail::sorted_pair_t> run;
            sketch_collector collector;
        } buffers;
        const auto ensure_replay_capacity = [&](std::uint64_t expected_run,
                                                std::uint64_t expected_sketches)
                -> bool {
            constexpr std::uint64_t max =
                (std::numeric_limits<std::uint64_t>::max)();
            if (expected_run
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())
                || expected_sketches
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
                return false;
            }
            const std::uint64_t run_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t sketch_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(MinimapSketchData)));
            const std::uint64_t retained_bytes =
                bioseq::parallelism::saturating_add(run_bytes, sketch_bytes);
            const std::uint64_t run_headroom =
                buffers.run.capacity() < expected_run
                    ? bioseq::parallelism::saturating_mul(
                        bioseq::parallelism::saturating_mul(
                            expected_run,
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::parallelism::kVectorPreallocationHeadroomFactor)
                    : 0u;
            const std::uint64_t sketch_headroom =
                buffers.collector.extract_sketches_data().capacity()
                        < expected_sketches
                    ? bioseq::parallelism::saturating_mul(
                        bioseq::parallelism::saturating_mul(
                            expected_sketches,
                            static_cast<std::uint64_t>(
                                sizeof(MinimapSketchData))),
                        bioseq::parallelism::kVectorPreallocationHeadroomFactor)
                    : 0u;
            const std::uint64_t allocation_peak =
                bioseq::parallelism::saturating_add(
                    retained_bytes,
                    bioseq::parallelism::saturating_add(
                        run_headroom, sketch_headroom));
            if (run_bytes == max || sketch_bytes == max
                || retained_bytes == max || run_headroom == max
                || sketch_headroom == max || allocation_peak == max) {
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
                    bioseq::parallelism::MemoryAdmissionController::instance()
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
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t exact_sketch =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(MinimapSketchData)));
            const std::uint64_t exact = bioseq::parallelism::saturating_add(
                exact_run, exact_sketch);
            if (exact_run == max || exact_sketch == max || exact == max
                || exact > buffers.permit.bytes()) {
                return false;
            }
            buffers.permit.shrink_to(exact);
            return true;
        };

        while (segment_lo < target_length) {
            if (!sql_integer_can_represent(segment_ordinal)) {
                return std::unexpected(
                    MinimizerGenerationWireError::segment_count);
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
                evidence.peak_core_span_bp = std::max(
                    evidence.peak_core_span_bp, core_hi - core_lo);
                buffers.run.clear();
                const std::uint64_t collection_units =
                    static_cast<std::uint64_t>(range->ce_u - range->cs_u);
                const std::uint64_t maximum_new_sketches =
                    bioseq::parallelism::saturating_mul(collection_units, lpu);
                // The checked Paper kernel's exact-tie matrix can emit the old
                // selected minimum plus at most W tied records for one input
                // candidate. Reserve that proven raw-output bound before the
                // kernel runs; canonical sort/unique below reduces it before
                // persistence. Other CPU collectors need at most one retained
                // vector slot per input bp on this bounded core route.
                constexpr std::uint64_t maximum_paper_records_per_candidate =
                    10u + 1u;
                const std::uint64_t expected_sketches =
                    backend == MinimizerSketchBackend::VulkanSegment
                        ? 0u
                        : (backend == MinimizerSketchBackend::PaperNtHashSimd
                            ? bioseq::parallelism::saturating_mul(
                                maximum_new_sketches,
                                maximum_paper_records_per_candidate)
                            : maximum_new_sketches);
                const std::uint64_t expected_run =
                    backend == MinimizerSketchBackend::VulkanSegment
                        ? core_hi - core_lo
                        : expected_sketches;
                if (!ensure_replay_capacity(expected_run, expected_sketches)) {
                    return std::unexpected(
                        MinimizerGenerationWireError::admission);
                }
                auto collected = collect_generation_core_run(
                    sequence,
                    backend,
                    *range,
                    buffers.collector,
                    buffers.run,
                    &evidence,
                    true);
                if (!collected) {
                    return std::unexpected(collected.error());
                }
                const std::uint64_t retained_after_collection =
                    bioseq::parallelism::saturating_add(
                        bioseq::parallelism::saturating_mul(
                            static_cast<std::uint64_t>(buffers.run.capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::parallelism::saturating_mul(
                            static_cast<std::uint64_t>(buffers.collector
                                .extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(MinimapSketchData))));
                if (retained_after_collection
                        == (std::numeric_limits<std::uint64_t>::max)()
                    || retained_after_collection > buffers.permit.bytes()) {
                    return std::unexpected(
                        MinimizerGenerationWireError::admission);
                }
                buffers.permit.shrink_to(retained_after_collection);
                detail::sort_pair_run(buffers.run);
                // A source occurrence selected by adjacent tied windows is one
                // persisted event. Canonicalize before spill so qualification,
                // authority evidence, and the encoder all count the same
                // `(hash, encoded_position)` stream. The external merge still
                // deduplicates across arbitrary runs as a defensive invariant.
                buffers.run.erase(
                    std::ranges::unique(buffers.run).begin(),
                    buffers.run.end());
                evidence.peak_core_events = std::max<std::uint64_t>(
                    evidence.peak_core_events, buffers.run.size());
                if (!buffers.run.empty()) {
                    const MpsSegmentSpillStorageBinding before_spill =
                        storage_binding;
                    storage.spill_sorted_segment_run(
                        static_cast<std::uint32_t>(segment_ordinal),
                        std::span<const detail::sorted_pair_t>{buffers.run});
                    storage_binding = storage.segment_spill_storage_binding();
                    if (!storage_binding.valid()
                        || storage_binding.generation_token
                            != before_spill.generation_token
                        || before_spill.mutation_version
                            == (std::numeric_limits<std::uint64_t>::max)()
                        || storage_binding.mutation_version
                            != before_spill.mutation_version + 1u) {
                        return std::unexpected(
                            MinimizerGenerationWireError::replay_mismatch);
                    }
                    MpsGenerationExecutionEvidence::add(
                        evidence.spill_run_count, 1u, evidence.saturated);
                    MpsGenerationExecutionEvidence::add(
                        evidence.spilled_event_count,
                        buffers.run.size(),
                        evidence.saturated);
                    if (evidence.saturated) {
                        return std::unexpected(
                            MinimizerGenerationWireError::arithmetic_overflow);
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
        // version and adopt a new full SHA-256 only after hashing every event.
        struct SpillQualification {
            std::uint64_t event_count = 0;
            bool saturated = false;
        } qualification;
        auto qualification_producer =
            MpsUnboundExternalSpillReplayProducer<S>{
                storage,
                target_length,
                position_segment_span_bp,
                minimizer_scheme};
        const auto qualified = qualification_producer.scan_sorted_events(
            position_segment_span_bp,
            CanonicalMinimizerEventReceiver{
                .context = &qualification,
                .emit_fn = +[](void* context,
                               const CanonicalMinimizerEvent& event)
                    -> std::expected<void, MinimizerGenerationWireError> {
                    auto& state = *static_cast<SpillQualification*>(context);
                    MpsGenerationExecutionEvidence::add(
                        state.event_count, 1u, state.saturated);
                    if (state.saturated) {
                        return std::unexpected(
                            MinimizerGenerationWireError::arithmetic_overflow);
                    }
                    (void)event;
                    return {};
                },
            });
        if (!qualified) {
            return std::unexpected(qualified.error());
        }
        if (qualification.event_count != evidence.spilled_event_count) {
            return std::unexpected(
                MinimizerGenerationWireError::duplicate_event);
        }
        const MpsGenerationExecutionEvidence qualification_evidence =
            qualification_producer.execution_evidence();
        evidence.spill_content_digest =
            qualification_evidence.replay_first_completed_content_digest;
        evidence.spill_qualification_scan_count =
            qualification_producer.scan_count();
        evidence.spill_qualification_merge_count =
            qualification_producer.merge_pass_count();
        const std::uint64_t operation_epoch =
            MpsGenerationExecutionEvidence::next_spill_operation_epoch();
        const std::uint64_t authority_seal =
            MpsGenerationExecutionEvidence::next_spill_authority_seal();
        if (operation_epoch == 0 || authority_seal == 0) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        evidence.spill_operation_epoch = operation_epoch;
        evidence.spill_storage_binding = storage_binding;
        auto authority = std::shared_ptr<const MpsSpillAuthority<S>>{
            new MpsSpillAuthority<S>{
                storage,
                target_length,
                position_segment_span_bp,
                evidence.spill_run_count,
                evidence.spilled_event_count,
                evidence.spill_content_digest,
                storage_binding,
                operation_epoch,
                authority_seal,
                minimizer_scheme}};
        evidence.bind_spill_authority(
            authority_seal,
            evidence.spilled_event_count,
            evidence.spill_content_digest,
            storage_binding,
            operation_epoch);
        evidence.spill_complete = true;
        evidence.capture_route_snapshot_();
        if (!evidence.spill_accepted()) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        return GenerationSpillResult{
            evidence.spill_run_count,
            evidence.exact_simd_proof,
            evidence.exact_simd_proof_present,
            minimizer_scheme,
            target_length,
            position_segment_span_bp,
            evidence,
            std::move(authority)};
    }

    [[nodiscard]] static auto make_generation_replay_producer(
            S& storage,
            std::uint64_t target_length,
            GenerationSpillResult&& spill,
            std::uint64_t position_segment_span_bp =
                kMinimizerPreferredSegmentSpanBp) {
        static_assert(MpsSegmentQualifiedSpillStorage<S>);
        MpsGenerationExecutionEvidence evidence = spill.execution_evidence_;
        std::shared_ptr<const MpsSpillAuthority<S>> authority =
            std::move(spill.authority_);
        const bool spill_identity_matches =
            authority != nullptr
            && authority->storage_ == &storage
            && authority->target_length_ == target_length
            && authority->initial_span_bp_ == position_segment_span_bp
            && authority->run_count_ == spill.run_count_
            && authority->event_count_ == evidence.spilled_event_count
            && authority->content_digest_ == evidence.spill_content_digest
            && authority->storage_binding_ == evidence.spill_storage_binding
            && storage.segment_spill_storage_binding()
                == authority->storage_binding_
            && authority->operation_epoch_ == evidence.spill_operation_epoch
            && authority->authority_seal_ != 0
            && authority->minimizer_scheme_ == spill.minimizer_scheme_
            && spill.target_length_ == target_length
            && spill.position_segment_span_bp_ == position_segment_span_bp
            && evidence.authority_matches(
                authority->authority_seal_,
                authority->event_count_,
                authority->content_digest_,
                authority->storage_binding_,
                authority->operation_epoch_);
        const bool evidence_identity_matches = spill_identity_matches
            && spill.minimizer_scheme_ == evidence.minimizer_scheme
            && spill.run_count_ == evidence.spill_run_count
            && spill.exact_simd_proof_present_
                == evidence.exact_simd_proof_present;
        if (!evidence_identity_matches) {
            evidence.backend_bound = false;
            authority.reset();
        }
        const MinimizerSchemeIdentity producer_scheme =
            evidence_identity_matches ? spill.minimizer_scheme_
                                      : MinimizerSchemeIdentity{};
        spill.poison_();
        return MpsExternalSpillGenerationReplayProducer<S>{
            storage, target_length, position_segment_span_bp,
            producer_scheme,
            evidence,
            std::move(authority),
            evidence.spill_storage_binding};
    }

    /// Concrete external-spill construction seam. Both the frozen Format-4
    /// layout and the canonical Format-5 shape layout consume the same sealed
    /// MPS spill. Other wire formats are not an implicit compatibility route.
    template <MinimizerGenerationRowSink Sink>
    [[nodiscard]] static std::expected<
        EncodedMinimizerGeneration,
        MinimizerGenerationWireError>
    encode_persisted_generation(
            S& storage,
            Sink& sink,
            GenerationSpillResult&& spill,
            const MinimizerGenerationEncodeOptions& options,
            const MinimizerGenerationFixedWidthLimits& limits = {},
            MpsGenerationExecutionEvidence* execution_evidence_out = nullptr) {
        if (execution_evidence_out != nullptr) {
            *execution_evidence_out = {};
        }
        const bool supported_mps_wire_format =
            options.wire_format == MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32
            || options.wire_format == MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local;
        if (!supported_mps_wire_format
            || options.window_size != 10 || options.kmer_size != 15
            || spill.position_segment_span_bp_
                != kMinimizerPreferredSegmentSpanBp
            || spill.authority_ == nullptr
            || spill.authority_->storage_ != &storage
            || storage.segment_spill_storage_binding()
                != spill.authority_->storage_binding_
            || !spill.execution_evidence_.spill_accepted()) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        auto producer = make_generation_replay_producer(
            storage,
            options.target_length,
            std::move(spill),
            kMinimizerPreferredSegmentSpanBp);
        auto encoded = encode_minimizer_generation(
            producer, sink, options, limits);
        MpsGenerationExecutionEvidence evidence =
            producer.execution_evidence();
        if (encoded) {
            evidence.manifest_position_count =
                encoded->manifest.position_count;
            evidence.manifest_position_segment_span_bp =
                encoded->manifest.position_segment_span_bp;
            evidence.manifest_bound =
                encoded->manifest.wire_format == options.wire_format
                && encoded->manifest.wire_format
                    == evidence.generation_wire_format
                && encoded->manifest.target_length == evidence.target_length
                && encoded->manifest.position_count
                    == evidence.spilled_event_count
                && encoded->manifest.kmer_size == evidence.kmer_size
                && encoded->manifest.window_size == evidence.window_size
                && encoded->manifest.minimizer_scheme_id
                    == evidence.minimizer_scheme.id
                && encoded->manifest.minimizer_scheme_version
                    == evidence.minimizer_scheme.version
                && encoded->manifest.producer_backend_id
                    == minimizer_producer_backend_identity_for_backend(
                        evidence.selected_backend).id
                && encoded->manifest.producer_backend_identity_version
                    == kMinimizerProducerBackendIdentityVersion;
            if (evidence.manifest_bound) {
                evidence.bind_manifest_authority();
                evidence.bind_publication_authority(*encoded);
                evidence.capture_replay_snapshot_();
            }
            if (!evidence.accepted()) {
                encoded = std::unexpected(
                    MinimizerGenerationWireError::compatibility_mismatch);
            }
        }
        if (execution_evidence_out != nullptr) {
            *execution_evidence_out = evidence;
        }
        return encoded;
    }

    template <class R, class Scanner>
    static void build(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend,
        ScalarMpsBuildExecutionProof* scalar_build_proof_out = nullptr) {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::size_t w = 10, k = 15;
        constexpr std::size_t lpu        = traits::letters_per_unit;
        constexpr std::size_t frag_units = Scanner::fragment_storage_units;
        constexpr std::size_t frag_bp    = frag_units * lpu;
        constexpr std::size_t W          = w + k - 2u;
        static_assert(frag_bp < (std::size_t{1} << 28), "fragment core positions fit 28-bit field");
        using detail::HDR; using detail::REC; using detail::BucketRecord;
        constexpr std::size_t kBucketTargetBytes = std::size_t{8} << 20;       // 8 MiB

        if (scalar_build_proof_out != nullptr) {
            *scalar_build_proof_out = {};
        }
        const bool capture_scalar_build_proof =
            scalar_build_proof_out != nullptr;
        ScalarMpsBuildExecutionProof scalar_build_proof{};
        if (capture_scalar_build_proof) {
            scalar_build_proof.bind_backend(
                backend,
                static_cast<std::uint32_t>(
                    sizeof(typename traits::storage_unit)),
                static_cast<std::uint32_t>(lpu));
        }

        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if ((std::size_t)seq.size() != (std::size_t)((total_bp + lpu - 1) / lpu))
            index_corruption_panic("minimizers_per_sequence build: scanner unit-span != ceil(total_bp/lpu)");

        // ---- Pass 0: collect globalised (kmer, encoded) pairs, spill sorted runs.
        const std::size_t run_buffer_bytes = storage.run_buffer_bytes();
        using pair_t = detail::sorted_pair_t;
        using detail::AdmittedPairRun;
        const auto make_core_range = [&seq](
                sequence_size_t core_lo,
                sequence_size_t core_hi) {
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, W);
            if (!range) {
                index_corruption_panic(
                    "minimizers_per_sequence build: invalid collection/core range");
            }
            return *range;
        };
        struct WorkerRangeResult {
            detail::AdmittedVectorRun<AdmittedPairRun> runs;
            detail::AdmittedVectorRun<SegmentUnitRange>
                deferred_for_backend_spill;
            ScalarMpsBuildExecutionProof scalar_build_proof{};
            std::exception_ptr error;
            SegmentUnitRange error_range{};
        };

        auto release_untransferred_results_noexcept =
            [](std::vector<WorkerRangeResult>& results) noexcept {
                for (WorkerRangeResult& result : results) {
                    for (AdmittedPairRun& run : result.runs.pairs) {
                        run.release_capacity_before_permit();
                    }
                    result.runs.release_capacity_before_permit();
                    result.deferred_for_backend_spill
                        .release_capacity_before_permit();
                }
            };

        // DOC0345 Patch E — under VulkanSegment the fused GPU sketch emits the
        // window's minimizers on-device; the globalised (kmer, encoded_pos)
        // pairs are sorted on the HOST, so there is no CPU minimizer COLLECT on
        // this path. The scalar branch is unchanged.
        auto collect_core_range_pairs =
            [&seq, backend](
                sequence_size_t core_lo,
                sequence_size_t core_hi,
                sketch_collector& collector,
                std::vector<pair_t>& out,
                ScalarMpsBuildExecutionProof* scalar_proof) {
            if (core_hi <= core_lo) {
                return;
            }
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, W);
            if (!range || !collect_generation_core_run(
                    seq,
                    backend,
                    *range,
                    collector,
                    out,
                    nullptr,
                    false,
                    scalar_proof)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collection/core geometry failed");
            }
        };

        auto collect_sort_and_spill_backend_scratch =
            [&](auto& self, SegmentUnitRange range) -> void {
                const std::uint64_t bp =
                    static_cast<std::uint64_t>(
                        range.core_hi_bp - range.core_lo_bp);
                const std::uint64_t pair_preallocation_bytes =
                    detail::admitted_vector_preallocation_bytes<pair_t>(bp);
                // The strict Vulkan segment leaf returns an owned pair run
                // carrying its single scratch/result permit. Do not reserve
                // that same GPU transaction again in this caller.
                const std::uint64_t backend_scratch_bytes = 0u;
                const std::uint64_t maximum_collector_sketches =
                    backend == MinimizerSketchBackend::VulkanSegment
                        ? 0u
                        : bioseq::parallelism::saturating_mul(
                            static_cast<std::uint64_t>(
                                range.ce_u - range.cs_u),
                            static_cast<std::uint64_t>(lpu));
                const std::uint64_t collector_headroom_bytes =
                    detail::admitted_vector_preallocation_bytes<
                        MinimapSketchData>(maximum_collector_sketches);
                const std::uint64_t admission_bytes =
                    bioseq::parallelism::saturating_add(
                        pair_preallocation_bytes,
                        bioseq::parallelism::saturating_add(
                            collector_headroom_bytes,
                            backend_scratch_bytes));
                if (backend == MinimizerSketchBackend::VulkanSegment) {
                    if constexpr (requires (S& s, std::uint64_t bytes) {
                                      s.demote_resident_runs_until_available(bytes);
                                  }) {
                        storage.demote_resident_runs_until_available(admission_bytes);
                    }
                }
                auto permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(admission_bytes);
                if (!permit) {
                    if constexpr (requires (S& s, std::uint64_t bytes) {
                                      s.demote_resident_runs_until_available(bytes);
                                  }) {
                        storage.demote_resident_runs_until_available(admission_bytes);
                        permit = bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(admission_bytes);
                    }
                }
                if (!permit) {
                    constexpr sequence_size_t kScalarMinimumCoreBp = 4096;
                    constexpr sequence_size_t kVulkanMinimumCoreBp = 256;
                    const sequence_size_t min_core_bp =
                        backend == MinimizerSketchBackend::VulkanSegment
                            ? kVulkanMinimumCoreBp
                            : kScalarMinimumCoreBp;
                    if (range.core_hi_bp - range.core_lo_bp > min_core_bp) {
                        const sequence_size_t mid =
                            range.core_lo_bp
                            + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid));
                        self(self, make_core_range(mid, range.core_hi_bp));
                        return;
                    }
                    // Never bypass the configured cap with an "emergency"
                    // heap vector.  The caller can retry after pressure clears.
                    throw std::bad_alloc{};
                }
                AdmittedPairRun scratch;
                scratch.permit = std::move(*permit);
                if (!detail::reserve_admitted_vector_run_capacity(
                        scratch,
                        bp,
                        bioseq::parallelism::saturating_add(
                            collector_headroom_bytes,
                            backend_scratch_bytes))) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: scratch pair capacity exceeded admission");
                }
                {
                    // Declared after `scratch`: collector capacity is destroyed
                    // before the shared permit can shrink or unwind.
                    sketch_collector collector;
                    if (maximum_collector_sketches != 0u) {
                        if (maximum_collector_sketches
                                > static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::size_t>::max)())) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: collector capacity overflow");
                        }
                        collector.reserve(static_cast<std::size_t>(
                            maximum_collector_sketches));
                    }
                    const std::uint64_t exact_collector_bytes =
                        bioseq::parallelism::saturating_mul(
                            static_cast<std::uint64_t>(collector
                                .extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(MinimapSketchData)));
                    const std::uint64_t live_before_collect =
                        bioseq::parallelism::saturating_add(
                            scratch.admitted_bytes,
                            bioseq::parallelism::saturating_add(
                                exact_collector_bytes,
                                backend_scratch_bytes));
                    if (exact_collector_bytes > collector_headroom_bytes
                        || live_before_collect > scratch.permit.bytes()) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: collector capacity exceeded admission");
                    }
                    scratch.permit.shrink_to(live_before_collect);
                    collect_core_range_pairs(
                        range.core_lo_bp,
                        range.core_hi_bp,
                        collector,
                        scratch.pairs,
                        capture_scalar_build_proof
                            ? &scalar_build_proof : nullptr);
                    const std::uint64_t live_after_collect =
                        bioseq::parallelism::saturating_add(
                            detail::admitted_vector_capacity_bytes(
                                scratch.pairs),
                            bioseq::parallelism::saturating_add(
                                bioseq::parallelism::saturating_mul(
                                    static_cast<std::uint64_t>(collector
                                        .extract_sketches_data().capacity()),
                                    static_cast<std::uint64_t>(
                                        sizeof(MinimapSketchData))),
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
                if (backend != MinimizerSketchBackend::VulkanSegment) {
                    detail::sort_pair_run(scratch.pairs);
                }
                storage.spill_sorted_run(
                    std::span<const pair_t>(
                        scratch.pairs.data(), scratch.pairs.size()));
                scratch.release_capacity_before_permit();
            };

        auto transfer_result_runs = [&](std::vector<WorkerRangeResult>& results) {
            for (WorkerRangeResult& result : results) {
                if (result.error) {
                    try {
                        std::rethrow_exception(result.error);
                    } catch (const ExactSimdIndexRuntimeError&) {
                        // Preserve the checked ExactSimd failure so the PG
                        // SQL boundary can translate it to 0A000. Converting
                        // it to index_corruption_panic would abort the backend
                        // and hide the original unsupported shape/status.
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
                if (capture_scalar_build_proof) {
                    scalar_build_proof.combine(result.scalar_build_proof);
                }
                for (AdmittedPairRun& run : result.runs.pairs) {
                    if (!run.pairs.empty()) {
                        storage.spill_sorted_run_owned_admitted(
                            std::move(run.pairs),
                            run.admitted_bytes,
                            std::move(run.permit));
                        run.admitted_bytes = 0;
                    }
                }
                for (SegmentUnitRange range
                     : result.deferred_for_backend_spill.pairs) {
                    collect_sort_and_spill_backend_scratch(
                        collect_sort_and_spill_backend_scratch, range);
                }
            }
            release_untransferred_results_noexcept(results);
        };

        auto collect_range_admitted =
            [&](auto& self, SegmentUnitRange range, WorkerRangeResult& result) -> void {
                if (result.error) {
                    return;
                }
                try {
                    const std::uint64_t bp =
                        static_cast<std::uint64_t>(
                            range.core_hi_bp - range.core_lo_bp);
                    const std::uint64_t pair_preallocation_bytes =
                        detail::admitted_vector_preallocation_bytes<pair_t>(bp);
                    const std::uint64_t backend_scratch_bytes = 0u;
                    const std::uint64_t maximum_collector_sketches =
                        backend == MinimizerSketchBackend::VulkanSegment
                            ? 0u
                            : bioseq::parallelism::saturating_mul(
                                static_cast<std::uint64_t>(
                                    range.ce_u - range.cs_u),
                                static_cast<std::uint64_t>(lpu));
                    const std::uint64_t collector_headroom_bytes =
                        detail::admitted_vector_preallocation_bytes<
                            MinimapSketchData>(maximum_collector_sketches);
                    const std::uint64_t admission_bytes =
                        bioseq::parallelism::saturating_add(
                            pair_preallocation_bytes,
                            bioseq::parallelism::saturating_add(
                                collector_headroom_bytes,
                                backend_scratch_bytes));
                    if (backend == MinimizerSketchBackend::VulkanSegment) {
                        if constexpr (requires (S& s, std::uint64_t bytes) {
                                          s.demote_resident_runs_until_available(bytes);
                                      }) {
                            storage.demote_resident_runs_until_available(admission_bytes);
                        }
                    }
                    auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(admission_bytes);
                    if (!permit) {
                        constexpr sequence_size_t kScalarMinCoreBp = 4096;
                        constexpr sequence_size_t kVulkanMinCoreBp = 256;
                        const sequence_size_t min_core_bp =
                            backend == MinimizerSketchBackend::VulkanSegment
                                ? kVulkanMinCoreBp
                                : kScalarMinCoreBp;
                        if (range.core_hi_bp - range.core_lo_bp <= min_core_bp) {
                            if (!detail::ensure_admitted_vector_run_capacity(
                                    result.deferred_for_backend_spill,
                                    result.deferred_for_backend_spill.pairs.size()
                                        + 1u)) {
                                throw std::bad_alloc{};
                            }
                            result.deferred_for_backend_spill.pairs.push_back(range);
                            return;
                        }
                        const sequence_size_t mid =
                            range.core_lo_bp
                            + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid), result);
                        self(self, make_core_range(mid, range.core_hi_bp), result);
                        return;
                    }

                    AdmittedPairRun run{};
                    run.permit = std::move(*permit);
                    if (!detail::reserve_admitted_vector_run_capacity(
                            run,
                            bp,
                            bioseq::parallelism::saturating_add(
                                collector_headroom_bytes,
                                backend_scratch_bytes))) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: worker pair capacity exceeded admission");
                    }
                    {
                        sketch_collector collector;
                        if (maximum_collector_sketches != 0u) {
                            if (maximum_collector_sketches
                                    > static_cast<std::uint64_t>(
                                        (std::numeric_limits<std::size_t>::max)())) {
                                index_corruption_panic(
                                    "minimizers_per_sequence build: worker collector overflow");
                            }
                            collector.reserve(static_cast<std::size_t>(
                                maximum_collector_sketches));
                        }
                        const std::uint64_t exact_collector_bytes =
                            bioseq::parallelism::saturating_mul(
                                static_cast<std::uint64_t>(collector
                                    .extract_sketches_data().capacity()),
                                static_cast<std::uint64_t>(
                                    sizeof(MinimapSketchData)));
                        const std::uint64_t live_before_collect =
                            bioseq::parallelism::saturating_add(
                                run.admitted_bytes,
                                bioseq::parallelism::saturating_add(
                                    exact_collector_bytes,
                                    backend_scratch_bytes));
                        if (exact_collector_bytes > collector_headroom_bytes
                            || live_before_collect > run.permit.bytes()) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: worker collector exceeded admission");
                        }
                        run.permit.shrink_to(live_before_collect);
                        collect_core_range_pairs(
                            range.core_lo_bp,
                            range.core_hi_bp,
                            collector,
                            run.pairs,
                            capture_scalar_build_proof
                                ? &result.scalar_build_proof : nullptr);
                        const std::uint64_t live_after_collect =
                            bioseq::parallelism::saturating_add(
                                detail::admitted_vector_capacity_bytes(run.pairs),
                                bioseq::parallelism::saturating_add(
                                    bioseq::parallelism::saturating_mul(
                                        static_cast<std::uint64_t>(collector
                                            .extract_sketches_data().capacity()),
                                        static_cast<std::uint64_t>(
                                            sizeof(MinimapSketchData))),
                                    backend_scratch_bytes));
                        if (live_after_collect > run.permit.bytes()) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: worker collection grew beyond admission");
                        }
                    }
                    // Backend scratch has returned; only vector capacity remains.
                    if (!detail::finish_admitted_vector_run_transient(run)) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: worker pair collection exceeded admission");
                    }
                    if (backend != MinimizerSketchBackend::VulkanSegment) {
                        detail::sort_pair_run(run.pairs);
                    }
                    detail::compact_admitted_pair_run_after_sort(run);
                    if (!detail::ensure_admitted_vector_run_capacity(
                            result.runs, result.runs.pairs.size() + 1u)) {
                        throw std::bad_alloc{};
                    }
                    result.runs.pairs.push_back(std::move(run));
                } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& err) {
                    const std::string_view message{err.what()};
                    const bool split_admission_decline =
                        message.starts_with(
                            "Vulkan segment minimizer pair build returned invalid ");
                    if (backend == MinimizerSketchBackend::VulkanSegment
                        && split_admission_decline) {
                        constexpr sequence_size_t kMinGpuCoreBp = 256;
                        if (range.core_hi_bp - range.core_lo_bp > kMinGpuCoreBp) {
                            const sequence_size_t mid =
                                range.core_lo_bp
                                + (range.core_hi_bp - range.core_lo_bp) / 2;
                            self(self, make_core_range(range.core_lo_bp, mid), result);
                            self(self, make_core_range(mid, range.core_hi_bp), result);
                            return;
                        }
                    }
                    result.error = std::current_exception();
                    result.error_range = range;
                } catch (const std::bad_alloc&) {
                    if (backend == MinimizerSketchBackend::VulkanSegment) {
                        constexpr sequence_size_t kMinGpuCoreBp = 256;
                        if (range.core_hi_bp - range.core_lo_bp > kMinGpuCoreBp) {
                            const sequence_size_t mid =
                                range.core_lo_bp
                                + (range.core_hi_bp - range.core_lo_bp) / 2;
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
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(frag_bp),
                static_cast<std::uint64_t>(sizeof(pair_t)));
        const std::size_t scalar_fragments_per_block = std::max<std::size_t>(
            1,
            run_buffer_bytes / std::max<std::uint64_t>(1, peak_bytes_per_fragment));
        // DOC0345 Patch E — one GPU window's local positions must fit the 28-bit
        // field, so clamp the per-block fragment count when collecting on GPU.
        const std::size_t fragments_per_block =
            backend == MinimizerSketchBackend::VulkanSegment
                ? detail::gpu_fragments_per_block(
                    scalar_fragments_per_block,
                    frag_bp,
                    2u * W)
                : scalar_fragments_per_block;
        detail::AdmittedVectorRun<SegmentUnitRange> admitted_window_ranges;
        const std::size_t maximum_window_ranges =
            nfrag == 0u
                ? 0u
                : (nfrag + fragments_per_block - 1u) / fragments_per_block;
        if (!detail::ensure_admitted_vector_run_capacity(
                admitted_window_ranges, maximum_window_ranges)) {
            index_corruption_panic(
                "minimizers_per_sequence build: window metadata admission failed");
        }
        std::vector<SegmentUnitRange>& window_ranges =
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

        auto collect_serial = [&]() {
            detail::AdmittedVectorRun<WorkerRangeResult> admitted_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_results, 1u)) {
                throw std::bad_alloc{};
            }
            admitted_results.pairs.resize(1u);
            std::vector<WorkerRangeResult>& results = admitted_results.pairs;
            for (SegmentUnitRange range : window_ranges) {
                results[0] = WorkerRangeResult{};
                collect_range_admitted(collect_range_admitted, range, results[0]);
                transfer_result_runs(results);
            }
        };

        auto& pool = bioseq::parallelism::GlobalPool::instance();
        // Vulkan serialises window submissions through one runtime queue. Both
        // host collectors are independent per range when the lazy scanner
        // advertises worker sharing.
        const bool may_parallel_collect =
            backend != MinimizerSketchBackend::VulkanSegment
            && bioseq::parallelism::task_parallel(pool.mode())
            && pool.max_concurrency() > 1
            && bioseq::storage::lazy_is_worker_shareable(seq)
            && nfrag >= bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT;

        if (!may_parallel_collect) {
            collect_serial();
        } else {
            detail::AdmittedVectorRun<WorkerRangeResult>
                admitted_local_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_local_results, window_ranges.size())) {
                throw std::bad_alloc{};
            }
            admitted_local_results.pairs.resize(window_ranges.size());
            std::vector<WorkerRangeResult>& local_results =
                admitted_local_results.pairs;
            try {
                bioseq::parallelism::parallel_for_pipeline(
                    0, local_results.size(), 1,
                    bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT,
                    bioseq::parallelism::TaskParallelRegion::fragment_build,
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
        detail::AdmittedVectorRun<MpsV2BucketDirEntry> admitted_dir;
        detail::AdmittedVectorRun<BucketRecord> admitted_ob_buckets;
        detail::AdmittedVectorRun<std::uint64_t> admitted_ob_positions;
        std::vector<MpsV2BucketDirEntry>& dir = admitted_dir.pairs;
        std::vector<BucketRecord>& ob_buckets = admitted_ob_buckets.pairs;
        std::vector<std::uint64_t>& ob_positions = admitted_ob_positions.pairs;
        std::uint64_t ob_lo = 0, ob_hi = 0;
        std::size_t ob_body_bytes = 0;                 // REC*nkmers + 8*npos
        index_sub_key_t next_bucket_id = 0;
        std::uint64_t total_distinct = 0, total_positions = 0;

        auto flush_bucket = [&]() {
            const index_sub_key_t sk = next_bucket_id++;
            if (sk >= TEMP_RUN_BASE) index_corruption_panic("minimizers_per_sequence: bucket id collided with reserved range");
            const std::size_t framed = HDR + ob_body_bytes;
            std::array<std::size_t,1> plan{ framed };
            storage.resize_chunks(sk, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(sk, [&](std::size_t c, char* outbuf, std::size_t cap){
                DEBUG_ASSERT(c == 0 && cap >= framed, "one bucket blob fits one chunk");
                detail::serialize_v2_blob(outbuf, total_bp, ob_buckets, ob_positions);
                return c;
            });
            DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < ob_lo, "directory not strictly ascending (flush)");
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_dir, dir.size() + 1u)) {
                index_corruption_panic(
                    "minimizers_per_sequence: directory admission failed");
            }
            dir.push_back({ ob_lo, ob_hi, sk });
            ob_buckets.clear(); ob_positions.clear(); ob_body_bytes = 0; ob_lo = 0; ob_hi = 0;
        };

        storage.merge_runs([&](std::uint64_t K, auto&& positions) {
            const std::uint64_t pcount = positions.total_count();
            if (pcount > UINT32_MAX) index_corruption_panic("merge_runs: kmer count > u32 (SerializedBucket.pos_count)");
            ++total_distinct; total_positions += pcount;
            const std::size_t kmerFramed = HDR + REC * 1 + sizeof(std::uint64_t) * pcount;
            if (kmerFramed > kBucketTargetBytes) {
                // heavy-hitter: split K across chunks (each a standalone 1-record v2 blob)
                if (!ob_buckets.empty()) flush_bucket();
                const index_sub_key_t sk = next_bucket_id++;
                if (sk >= TEMP_RUN_BASE) index_corruption_panic("minimizers_per_sequence: bucket id collided with reserved range");
                detail::AdmittedVectorRun<std::uint64_t> admitted_kpos;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_kpos, static_cast<std::size_t>(pcount))) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy positions admission failed");
                }
                std::vector<std::uint64_t>& kpos = admitted_kpos.pairs;
                positions.for_each_batch([&](std::span<const std::uint64_t> b){
                    if (b.size()
                            > (std::numeric_limits<std::size_t>::max)()
                                - kpos.size()) {
                        index_corruption_panic(
                            "minimizers_per_sequence: heavy positions overflow");
                    }
                    kpos.insert(kpos.end(), b.begin(), b.end());
                });
                const std::size_t per = (kBucketTargetBytes - HDR - REC) / sizeof(std::uint64_t);
                const std::size_t plan_count =
                    (static_cast<std::size_t>(pcount) + per - 1u) / per;
                detail::AdmittedVectorRun<std::size_t> admitted_plan;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_plan, plan_count)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy plan admission failed");
                }
                std::vector<std::size_t>& plan = admitted_plan.pairs;
                for (std::size_t off = 0; off < (std::size_t)pcount; off += per)
                    plan.push_back(HDR + REC + sizeof(std::uint64_t) * std::min<std::size_t>(per, (std::size_t)pcount - off));
                storage.resize_chunks(sk, std::span<const std::size_t>(plan));
                storage.template write_to_each<char>(sk, [&](std::size_t c, char* outbuf, std::size_t){
                    const std::size_t start = c * per;
                    const std::size_t cnt = std::min<std::size_t>(per, (std::size_t)pcount - start);
                    BucketRecord rec{ K, 0u, (std::uint32_t)cnt };
                    detail::serialize_v2_blob(outbuf, total_bp, std::span<const BucketRecord>(&rec, 1),
                                              std::span<const std::uint64_t>(kpos.data() + start, cnt));
                    return c;
                });
                DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < K, "directory not strictly ascending (heavy-hitter)");
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_dir, dir.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: directory admission failed");
                }
                dir.push_back({ K, K, sk });
            } else {
                const std::size_t add = REC + sizeof(std::uint64_t) * pcount;
                if (HDR + ob_body_bytes + add > kBucketTargetBytes && !ob_buckets.empty()) flush_bucket();
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_positions,
                        ob_positions.size() + static_cast<std::size_t>(pcount))
                    || !detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_buckets, ob_buckets.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: bucket assembly admission failed");
                }
                const std::uint32_t off = (std::uint32_t)ob_positions.size();
                positions.for_each_batch([&](std::span<const std::uint64_t> b){ ob_positions.insert(ob_positions.end(), b.begin(), b.end()); });
                ob_buckets.push_back(BucketRecord{ K, off, (std::uint32_t)pcount });
                ob_hi = K; if (ob_buckets.size() == 1) ob_lo = K;
                ob_body_bytes += add;
            }
        });
        if (!ob_buckets.empty()) flush_bucket();

        // ---- write directory LAST.
        {
            const std::size_t dir_bytes = sizeof(MpsV2BucketDirectory) + sizeof(MpsV2BucketDirEntry) * dir.size();
            std::array<std::size_t,1> plan{ dir_bytes };
            storage.resize_chunks(DIRECTORY_SUB_KEY, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(DIRECTORY_SUB_KEY, [&](std::size_t c, char* outbuf, std::size_t){
                MpsV2BucketDirectory h;
                h.magic = MpsV2BucketDirectory::kDirMagic; h.version = MpsV2BucketDirectory::kDirVersion;
                h.flags = backend == MinimizerSketchBackend::VulkanSegment
                    && bioseq::gpu::vulkan::alphabet_kernel_kind_of_traits<traits>()
                        != bioseq::gpu::vulkan::AlphabetKernelKind::dna4_nt4
                    ? MpsV2BucketDirectory::kBuiltFromPhaseC
                    : 0u;
                h.num_buckets = dir.size(); h.total_distinct_kmers = total_distinct; h.total_positions = total_positions;
                std::memcpy(outbuf, &h, sizeof(h));
                if (!dir.empty()) std::memcpy(outbuf + sizeof(h), dir.data(), sizeof(MpsV2BucketDirEntry) * dir.size());
                return c;
            });
        }

        // ---- Pass 2: cleanup.
        if constexpr (requires (S& s) { s.snapshot_sorted_run_stats_before_release(); }) {
            storage.snapshot_sorted_run_stats_before_release();
        }
        storage.release_runs();
        if (capture_scalar_build_proof) {
            scalar_build_proof.record_completed_build();
            *scalar_build_proof_out = scalar_build_proof;
        }
    }

    // DOC0345 Patch E — scalar-default overload delegates to the explicit backend.
    template <class R, class Scanner>
    static void build(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
        build(storage, seq, MinimizerSketchBackend::Scalar);
    }

    template <class R, class Scanner>
    static void rebuild(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend)
    {
        storage.clear_entity();
        build(storage, seq, backend);
    }

    template <class R, class Scanner>
    static void rebuild(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
        rebuild(storage, seq, MinimizerSketchBackend::Scalar);
    }

    /// query-time: load ONLY the buckets containing any query kmer (§10.5).
    static MinimizerHashmapFragmentIndex assemble_for_query(
            S& storage, std::span<const MinimapSketchData> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            MinimizerAssemblyScratch& scratch) {
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
    static MinimizerHashmapFragmentIndex assemble_for_query_segment(
            S& storage, std::span<const MinimapSketchData> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            MinimizerAssemblyScratch& scratch) {
        using detail::HDR; using detail::REC;
        scratch.clear_query();
        clip_hi_bp = std::min(clip_hi_bp, target_len);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }
        // 1. read the directory into an OWNED buffer (DirView copies it).
        const char* dblob = storage.template read_chunk<char>(DIRECTORY_SUB_KEY, 0);
        const std::size_t dbytes = storage.chunk_byte_size(DIRECTORY_SUB_KEY, 0);
        storage.validate_minimizer_directory_or_error(dblob, dbytes);
        MpsV2DirView dir{ dblob, dbytes };
        const std::uint32_t max_occ_threshold =
            detail::effective_max_occ_threshold(max_occ, dir.built_from_phase_c());
        // 2. collect distinct query kmers + the distinct bucket sub_keys they touch.
        auto& qkmers = scratch.query_kmers;
        auto& wanted = scratch.wanted_sub_keys;
        for (const auto& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
            if (const MpsV2BucketDirEntry* e = dir.find(qm.kmer())) wanted.push_back(e->sub_key);
        }
        auto sort_unique = [](auto& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
        sort_unique(qkmers);
        sort_unique(wanted);
        // 3. read only those buckets; deserialise; drop kmers with global count > max_occ.
        auto& pairs = scratch.pairs;
        auto emit_kept = [&](std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos){
            if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer)) return;   // clip to QUERY kmers
            if (pos_count > max_occ_threshold) return;                              // drop over-frequent kmer
            for (std::uint32_t j = 0; j < pos_count; ++j) {
                const sequence_size_t pos_bp =
                    static_cast<sequence_size_t>(gpos[j] >> 1);
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
                // HEAVY-HITTER bucket: a single kmer K. Exact count from metadata alone.
                std::size_t exact = 0;
                for (std::size_t c = 0; c < nc; ++c) {
                    storage.validate_minimizer_chunk_metadata_or_error(sk, c, HDR + REC, alignof(std::uint64_t));
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    exact += (cb - HDR - REC) / sizeof(std::uint64_t);
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
        MinimizerHashmapFragmentIndex idx; idx.build_from_pairs(pairs, target_len);
        return idx;
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<MinimizerSequenceMergedIndex> {
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
    static constexpr bool allows_update_on_insert = true;   // opt-in external sort; not a fresh-table default
};
static_assert(requires { index_descriptor<MinimizerSequenceMergedIndex>::index_name; },
              "MinimizerSequenceMergedIndex index_descriptor must be exported");
}
