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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
export module bioseq.indexing.minimizer_sequence;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;            // MinimizerHashmapFragmentIndex, build_from_pairs, wire_for_each_*
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, MinimapSketchData, MinimizerSketchBackend
import bioseq.sequences.kmers.vulkan;                // DOC0345 Patch E — collect_sorted_minimizer_pairs_vulkan_segment_or_throw
import bioseq.sequences.kmers.hashers.minimap_int;   // minimap_int_hasher
import bioseq.storage.lazy;                          // lazy_compact_sequence
import bioseq.storage.size;                          // sequence_size_t
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
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

// ---- on-disk directory layout (§10.2) ------------------------------------
struct MinimizerBucketDirEntry {
    std::uint64_t lo_kmer;       // inclusive smallest kmer in the bucket
    std::uint64_t hi_kmer;       // inclusive largest kmer in the bucket
    std::int64_t  sub_key;       // bucket's storage sub_key
};
static_assert(sizeof(MinimizerBucketDirEntry) == 24 && alignof(MinimizerBucketDirEntry) == 8);

struct MinimizerBucketDirectory {
    static constexpr std::uint32_t kDirMagic   = 0xB1000002u;
    static constexpr std::uint32_t kDirVersion = 1u;
    std::uint32_t magic   = kDirMagic;
    std::uint32_t version = kDirVersion;
    std::uint64_t num_buckets = 0;
    bioseq::storage::sequence_size_t total_distinct_kmers = 0;
    bioseq::storage::sequence_size_t total_positions      = 0;
    // followed by MinimizerBucketDirEntry[num_buckets]
};
static_assert(sizeof(MinimizerBucketDirectory) == 32 && alignof(MinimizerBucketDirectory) == 8,
              "directory wire header layout pinned");
static_assert(sizeof(bioseq::storage::sequence_size_t) == 8, "directory wire fields assume 8-byte sequence_size_t");
static_assert(offsetof(MinimizerBucketDirectory, num_buckets) == 8
           && offsetof(MinimizerBucketDirectory, total_distinct_kmers) == 16
           && offsetof(MinimizerBucketDirectory, total_positions) == 24,
              "directory header field offsets pinned");
static_assert(offsetof(MinimizerBucketDirEntry, sub_key) == 16, "dir entry field offsets pinned");

/// Binary-search view over the directory chunk. Copies the blob into an owned
/// 8-aligned int64 vector so per-bucket read_chunk does not invalidate it.
struct DirView {
    std::vector<std::int64_t> owned;
    const MinimizerBucketDirectory* hdr = nullptr;
    const MinimizerBucketDirEntry* entries = nullptr;
    std::size_t n = 0;
    DirView(const char* blob, std::size_t bytes) {
        if (bytes < sizeof(MinimizerBucketDirectory)) index_corruption_panic("directory: truncated");
        owned.resize((bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t));
        std::memcpy(owned.data(), blob, bytes);
        hdr = reinterpret_cast<const MinimizerBucketDirectory*>(owned.data());
        if (hdr->magic != MinimizerBucketDirectory::kDirMagic || hdr->version != MinimizerBucketDirectory::kDirVersion)
            index_corruption_panic("directory: magic/version");
        n = static_cast<std::size_t>(hdr->num_buckets);
        const std::size_t expect = sizeof(MinimizerBucketDirectory) + sizeof(MinimizerBucketDirEntry) * n;
        if (bytes != expect) index_corruption_panic("directory: size mismatch");
        entries = reinterpret_cast<const MinimizerBucketDirEntry*>(
            reinterpret_cast<const char*>(owned.data()) + sizeof(MinimizerBucketDirectory));
        for (std::size_t i = 1; i < n; ++i)
            if (entries[i].lo_kmer <= entries[i-1].hi_kmer)
                index_corruption_panic("directory: not ascending/non-overlapping");
    }
    [[nodiscard]] const MinimizerBucketDirEntry* find(std::uint64_t kmer) const {
        std::size_t lo = 0, hi = n;
        while (lo < hi) { std::size_t mid = (lo + hi) / 2; if (entries[mid].hi_kmer < kmer) lo = mid + 1; else hi = mid; }
        if (lo < n && entries[lo].lo_kmer <= kmer && kmer <= entries[lo].hi_kmer) return &entries[lo];
        return nullptr;
    }
};

namespace detail {
using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;
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
    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(key_bytes);
    if (!permit) {
        std::sort(pairs.begin(), pairs.end());
        return;
    }

    std::vector<hwy::uint128_t> keys;
    keys.reserve(pairs.size());
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
    const std::size_t allowed_core_bp = max_bp - halo_bp;
    const std::size_t by_pos_field =
        std::max<std::size_t>(1u, allowed_core_bp / frag_bp);
    return std::max<std::size_t>(
        1u,
        std::min<std::size_t>(scalar_fragments_per_block, by_pos_field));
}
}  // namespace detail

template <class S>
struct MinimizerSequenceMergedIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
    static_assert(is_external_sort_spill_storage<S>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
    using storage_type = S;

    template <class R, class Scanner>
    static void build(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend) {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::size_t w = 10, k = 15;
        constexpr std::size_t lpu        = traits::letters_per_unit;
        constexpr std::size_t frag_units = Scanner::fragment_storage_units;
        constexpr std::size_t frag_bp    = frag_units * lpu;
        constexpr std::size_t W          = w + k;
        static_assert(frag_bp < (std::size_t{1} << 28), "fragment core positions fit 28-bit field");
        using detail::HDR; using detail::REC; using detail::BucketRecord;
        constexpr std::size_t kBucketTargetBytes = std::size_t{8} << 20;       // 8 MiB

        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if ((std::size_t)seq.size() != (std::size_t)((total_bp + lpu - 1) / lpu))
            index_corruption_panic("minimizers_per_sequence build: scanner unit-span != ceil(total_bp/lpu)");

        // ---- Pass 0: collect globalised (kmer, encoded) pairs, spill sorted runs.
        const std::size_t run_buffer_bytes = storage.run_buffer_bytes();
        const std::size_t size_u = (std::size_t)((total_bp + lpu - 1) / lpu);

        using pair_t = detail::sorted_pair_t;
        struct CoreRange {
            sequence_size_t core_lo = 0;
            sequence_size_t core_hi = 0;
        };
        struct AdmittedPairRun {
            std::vector<pair_t> pairs;
            std::uint64_t admitted_bytes = 0;
            bioseq::parallelism::MemoryPermit permit;
        };
        struct WorkerRangeResult {
            std::vector<AdmittedPairRun> runs;
            std::vector<CoreRange> deferred_for_backend_spill;
            std::exception_ptr error;
        };

        auto release_untransferred_results_noexcept =
            [](std::vector<WorkerRangeResult>& results) noexcept {
                for (WorkerRangeResult& result : results) {
                    for (AdmittedPairRun& run : result.runs) {
                        run.permit.release();
                        std::vector<pair_t>().swap(run.pairs);
                        run.admitted_bytes = 0;
                    }
                }
            };

        auto reserve_pairs_or_panic = [](std::vector<pair_t>& out, std::uint64_t count) {
            if (count > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                index_corruption_panic("minimizers_per_sequence build: admitted range too large");
            }
            out.reserve(static_cast<std::size_t>(count));
        };

        // DOC0345 Patch E — under VulkanSegment the GPU (Phase B sketch) emits
        // the window's minimizers on-device; the globalised (kmer, encoded_pos)
        // pairs are sorted on the HOST (the tiled GPU emit+sort over-emits for
        // dna5 and is bypassed — see DOC0348), so there is no CPU minimizer
        // COLLECT on this path. The scalar branch is unchanged.
        auto collect_core_range_pairs =
            [&seq, total_bp, size_u, backend](
                sequence_size_t core_lo,
                sequence_size_t core_hi,
                sketch_collector& collector,
                std::vector<pair_t>& out) {
            if (core_hi <= core_lo) {
                return;
            }

            const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
            const sequence_size_t coll_hi =
                std::min(core_hi + static_cast<sequence_size_t>(W), total_bp);
            const std::size_t lo_unit = static_cast<std::size_t>(coll_lo / lpu);
            const std::size_t hi_unit = std::min(
                static_cast<std::size_t>((coll_hi + lpu - 1) / lpu),
                size_u);
            const sequence_size_t emit_base =
                static_cast<sequence_size_t>(lo_unit) * lpu;
            const sequence_size_t collect_bp =
                coll_hi > emit_base ? coll_hi - emit_base : 0;
            if (collect_bp < static_cast<sequence_size_t>(W)) {
                return;
            }

            if (backend == MinimizerSketchBackend::VulkanSegment) {
                std::vector<pair_t> gpu_pairs =
                    bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                        seq,
                        lo_unit,
                        hi_unit,
                        emit_base,
                        core_lo,
                        core_hi,
                        w,
                        k);
                out.insert(
                    out.end(),
                    std::make_move_iterator(gpu_pairs.begin()),
                    std::make_move_iterator(gpu_pairs.end()));
                return;
            }

            collector.clear();
            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<sequence_size_t>(lo_unit);
            auto end_iter = seq.cbegin();
            end_iter += static_cast<sequence_size_t>(hi_unit);
            collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
                collector,
                seq,
                begin_iter,
                end_iter,
                w,
                k);
            for (const MinimapSketchData& minimizer : collector.extract_sketches_data()) {
                const sequence_size_t global_pos =
                    static_cast<sequence_size_t>(minimizer.last_pos()) + emit_base;
                if (global_pos < core_lo || global_pos >= core_hi) {
                    continue;
                }
                out.emplace_back(
                    minimizer.kmer(),
                    (static_cast<std::uint64_t>(global_pos) << 1)
                        | static_cast<std::uint64_t>(minimizer.strand()));
            }
        };

        auto compact_admitted_run_after_sort = [](AdmittedPairRun& run) {
            const std::uint64_t used_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(run.pairs.size()),
                    static_cast<std::uint64_t>(sizeof(pair_t)));
            auto shrink_permit_to_capacity = [&]() {
                const std::uint64_t keep_bytes =
                    bioseq::parallelism::saturating_mul(
                        static_cast<std::uint64_t>(run.pairs.capacity()),
                        static_cast<std::uint64_t>(sizeof(pair_t)));
                if (run.admitted_bytes > keep_bytes) {
                    run.permit.shrink_to(keep_bytes);
                    run.admitted_bytes = keep_bytes;
                }
            };

            if (used_bytes <= run.admitted_bytes / 2) {
                std::vector<pair_t> compact;
                compact.reserve(run.pairs.size());
                compact.insert(
                    compact.end(),
                    std::make_move_iterator(run.pairs.begin()),
                    std::make_move_iterator(run.pairs.end()));
                run.pairs.swap(compact);
                shrink_permit_to_capacity();
                return;
            }
            shrink_permit_to_capacity();
        };

        auto collect_sort_and_spill_backend_scratch =
            [&](auto& self, CoreRange range) -> void {
                const std::uint64_t bp =
                    static_cast<std::uint64_t>(range.core_hi - range.core_lo);
                const std::uint64_t reserve_bytes =
                    bioseq::parallelism::saturating_mul(
                        bp, static_cast<std::uint64_t>(sizeof(pair_t)));
                auto permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(reserve_bytes);
                if (!permit) {
                    if constexpr (requires (S& s, std::uint64_t bytes) {
                                      s.demote_resident_runs_until_available(bytes);
                                  }) {
                        storage.demote_resident_runs_until_available(reserve_bytes);
                        permit = bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(reserve_bytes);
                    }
                }
                if (!permit) {
                    constexpr sequence_size_t kEmergencyUnadmittedCoreBp = 4096;
                    if (range.core_hi - range.core_lo > kEmergencyUnadmittedCoreBp) {
                        const sequence_size_t mid =
                            range.core_lo + (range.core_hi - range.core_lo) / 2;
                        self(self, CoreRange{range.core_lo, mid});
                        self(self, CoreRange{mid, range.core_hi});
                        return;
                    }
                    std::vector<pair_t> scratch;
                    reserve_pairs_or_panic(scratch, reserve_bytes / sizeof(pair_t));
                    sketch_collector collector;
                    collect_core_range_pairs(
                        range.core_lo, range.core_hi, collector, scratch);
                    if (backend == MinimizerSketchBackend::Scalar) {
                        detail::sort_pair_run(scratch);
                    }
                    storage.spill_sorted_run(
                        std::span<const pair_t>(scratch.data(), scratch.size()));
                    return;
                }
                std::vector<pair_t> scratch;
                reserve_pairs_or_panic(scratch, reserve_bytes / sizeof(pair_t));
                sketch_collector collector;
                collect_core_range_pairs(
                    range.core_lo, range.core_hi, collector, scratch);
                if (backend == MinimizerSketchBackend::Scalar) {
                    detail::sort_pair_run(scratch);
                }
                storage.spill_sorted_run(
                    std::span<const pair_t>(scratch.data(), scratch.size()));
                permit->release();
            };

        auto transfer_result_runs = [&](std::vector<WorkerRangeResult>& results) {
            for (WorkerRangeResult& result : results) {
                if (result.error) {
                    try {
                        std::rethrow_exception(result.error);
                    } catch (const std::exception& e) {
                        (void)e;
                        index_corruption_panic(
                            "bioseq minimizers_per_sequence worker failed");
                    } catch (...) {
                        index_corruption_panic(
                            "bioseq minimizers_per_sequence worker failed");
                    }
                }
                for (AdmittedPairRun& run : result.runs) {
                    if (!run.pairs.empty()) {
                        storage.spill_sorted_run_owned_admitted(
                            std::move(run.pairs),
                            run.admitted_bytes,
                            std::move(run.permit));
                        run.admitted_bytes = 0;
                    }
                }
                for (CoreRange range : result.deferred_for_backend_spill) {
                    collect_sort_and_spill_backend_scratch(
                        collect_sort_and_spill_backend_scratch, range);
                }
            }
            release_untransferred_results_noexcept(results);
        };

        auto collect_range_admitted =
            [&](auto& self, CoreRange range, WorkerRangeResult& result) -> void {
                if (result.error) {
                    return;
                }
                try {
                    const std::uint64_t bp =
                        static_cast<std::uint64_t>(range.core_hi - range.core_lo);
                    const std::uint64_t reserve_bytes =
                        bioseq::parallelism::saturating_mul(
                            bp, static_cast<std::uint64_t>(sizeof(pair_t)));
                    auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(reserve_bytes);
                    if (!permit) {
                        constexpr sequence_size_t kMinCoreBp = 4096;
                        if (range.core_hi - range.core_lo <= kMinCoreBp) {
                            result.deferred_for_backend_spill.push_back(range);
                            return;
                        }
                        const sequence_size_t mid =
                            range.core_lo + (range.core_hi - range.core_lo) / 2;
                        self(self, CoreRange{range.core_lo, mid}, result);
                        self(self, CoreRange{mid, range.core_hi}, result);
                        return;
                    }

                    AdmittedPairRun run{};
                    run.admitted_bytes = reserve_bytes;
                    run.permit = std::move(*permit);
                    reserve_pairs_or_panic(run.pairs, reserve_bytes / sizeof(pair_t));
                    sketch_collector collector;
                    collect_core_range_pairs(
                        range.core_lo, range.core_hi, collector, run.pairs);
                    if (backend == MinimizerSketchBackend::Scalar) {
                        detail::sort_pair_run(run.pairs);
                    }
                    compact_admitted_run_after_sort(run);
                    result.runs.push_back(std::move(run));
                } catch (...) {
                    result.error = std::current_exception();
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
        std::vector<CoreRange> window_ranges;
        for (std::size_t frag_lo = 0; frag_lo < nfrag; frag_lo += fragments_per_block) {
            const std::size_t frag_hi = std::min(nfrag, frag_lo + fragments_per_block);
            const sequence_size_t core_lo = static_cast<sequence_size_t>(frag_lo) * frag_bp;
            const sequence_size_t core_hi = std::min(
                static_cast<sequence_size_t>(frag_hi) * frag_bp, total_bp);
            if (core_hi > core_lo) {
                window_ranges.push_back(CoreRange{core_lo, core_hi});
            }
        }

        auto collect_serial = [&]() {
            std::vector<WorkerRangeResult> results(1);
            sketch_collector collector;
            (void)collector;
            for (CoreRange range : window_ranges) {
                results[0] = WorkerRangeResult{};
                collect_range_admitted(collect_range_admitted, range, results[0]);
                transfer_result_runs(results);
            }
        };

        auto& pool = bioseq::parallelism::GlobalPool::instance();
        // DOC0345 Patch E — Vulkan serialises window submissions through the one
        // runtime queue, so only the scalar backend may collect in parallel.
        const bool may_parallel_collect =
            backend == MinimizerSketchBackend::Scalar
            && bioseq::parallelism::task_parallel(pool.mode())
            && pool.max_concurrency() > 1
            && bioseq::storage::lazy_is_worker_shareable(seq)
            && nfrag >= bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT;

        if (!may_parallel_collect) {
            collect_serial();
        } else {
            std::vector<WorkerRangeResult> local_results(window_ranges.size());
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
        std::vector<MinimizerBucketDirEntry> dir;
        std::vector<BucketRecord> ob_buckets;          // output bucket bucket-records
        std::vector<std::uint64_t> ob_positions;       // output bucket flat positions
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
                std::vector<std::uint64_t> kpos; kpos.reserve((std::size_t)pcount);
                positions.for_each_batch([&](std::span<const std::uint64_t> b){ kpos.insert(kpos.end(), b.begin(), b.end()); });
                const std::size_t per = (kBucketTargetBytes - HDR - REC) / sizeof(std::uint64_t);
                std::vector<std::size_t> plan;
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
                dir.push_back({ K, K, sk });
            } else {
                const std::size_t add = REC + sizeof(std::uint64_t) * pcount;
                if (HDR + ob_body_bytes + add > kBucketTargetBytes && !ob_buckets.empty()) flush_bucket();
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
            const std::size_t dir_bytes = sizeof(MinimizerBucketDirectory) + sizeof(MinimizerBucketDirEntry) * dir.size();
            std::array<std::size_t,1> plan{ dir_bytes };
            storage.resize_chunks(DIRECTORY_SUB_KEY, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(DIRECTORY_SUB_KEY, [&](std::size_t c, char* outbuf, std::size_t){
                MinimizerBucketDirectory h;
                h.magic = MinimizerBucketDirectory::kDirMagic; h.version = MinimizerBucketDirectory::kDirVersion;
                h.num_buckets = dir.size(); h.total_distinct_kmers = total_distinct; h.total_positions = total_positions;
                std::memcpy(outbuf, &h, sizeof(h));
                if (!dir.empty()) std::memcpy(outbuf + sizeof(h), dir.data(), sizeof(MinimizerBucketDirEntry) * dir.size());
                return c;
            });
        }

        // ---- Pass 2: cleanup.
        if constexpr (requires (S& s) { s.snapshot_sorted_run_stats_before_release(); }) {
            storage.snapshot_sorted_run_stats_before_release();
        }
        storage.release_runs();
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
        using detail::HDR; using detail::REC;
        scratch.clear_query();
        // 1. read the directory into an OWNED buffer (DirView copies it).
        const char* dblob = storage.template read_chunk<char>(DIRECTORY_SUB_KEY, 0);
        const std::size_t dbytes = storage.chunk_byte_size(DIRECTORY_SUB_KEY, 0);
        storage.validate_minimizer_directory_or_error(dblob, dbytes);
        DirView dir{ dblob, dbytes };
        // 2. collect distinct query kmers + the distinct bucket sub_keys they touch.
        auto& qkmers = scratch.query_kmers;
        auto& wanted = scratch.wanted_sub_keys;
        for (const auto& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
            if (const MinimizerBucketDirEntry* e = dir.find(qm.kmer())) wanted.push_back(e->sub_key);
        }
        auto sort_unique = [](auto& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
        sort_unique(qkmers);
        sort_unique(wanted);
        // 3. read only those buckets; deserialise; drop kmers with global count > max_occ.
        auto& pairs = scratch.pairs;
        auto emit_kept = [&](std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos){
            if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer)) return;   // clip to QUERY kmers
            if (pos_count > (std::uint32_t)max_occ) return;                         // drop over-frequent kmer
            for (std::uint32_t j = 0; j < pos_count; ++j) pairs.emplace_back(kmer, gpos[j]);
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
                if (exact > (std::size_t)max_occ) continue;        // drop over-frequent kmer
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
