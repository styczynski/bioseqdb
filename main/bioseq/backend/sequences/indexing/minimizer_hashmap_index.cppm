/**
 * @file minimizer_hashmap_index.cppm
 * @brief DOC0279 §9 — the relocated `TargetIndex` (now
 *        `MinimizerHashmapFragmentIndex`, storage-free, the single pipeline
 *        IndexT) + the storage-bound `MinimizersPerFragmentIndex<S>` builder.
 *
 * NO_POSTGRES. Corruption guards call `index_corruption_panic` (bioseq_log<DBG>+abort),
 * never elog/bioseq_log_pg(no-ops here). Storage-side validation goes through
 * `storage.validate_minimizer_*`/`storage.index_error` (duck-typed on S).
 */
module;
#include "common.h"                       // DEBUG_ASSERT, bioseq_log<DBG>*
#include <hwy/highway.h>                   // ONLY for the HWY_NAMESPACE macro
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>
#include <utility>
export module bioseq.indexing.minimizer_hashmap;
import bioseq.common.mlogging;                                     // bioseq_log_progress<DBG>(enable_logging)
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, MinimapSketchData, MinimizerSketchBackend
import bioseq.sequences.kmers.vulkan;                // DOC0345 Patch F — collect_sorted_minimizer_pairs_vulkan_segment_or_throw
import bioseq.sequences.kmers.hashers.minimap_int;   // minimap_int_hasher
import bioseq.storage.lazy;                          // lazy_compact_sequence, make_lazy_in_memory
import bioseq.storage.size;                          // sequence_size_t
import bioseq.common.radix_sort;                  // bioseq::sorting::radix_sort
import bioseq.instr;                                 // instrumentation region scoping
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::indexing::detail {
struct MinimizerHashmapFragmentIndexTable;
}

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers_view;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;   // DOC0345 Patch F
using bioseq::storage::sequence_size_t;
using bioseq::storage::lazy_compact_sequence;

[[noreturn]] [[gnu::cold]] void index_corruption_panic(std::string_view msg);  // bioseq_log<DBG>+abort (NO_POSTGRES-safe)

// (1) the lookup structure — the old TargetIndex, renamed & relocated, storage-free
struct MinimizerHashmapFragmentIndex {
private:
    std::unique_ptr<detail::MinimizerHashmapFragmentIndexTable> table_;

    void clear_table();
    void reserve_table(std::size_t bucket_count);
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

public:
    std::vector<std::uint64_t> positions;        // (last_pos<<1)|strand, full uint64
    sequence_size_t target_len = 0;

    MinimizerHashmapFragmentIndex();
    ~MinimizerHashmapFragmentIndex();
    MinimizerHashmapFragmentIndex(const MinimizerHashmapFragmentIndex& other);
    MinimizerHashmapFragmentIndex& operator=(const MinimizerHashmapFragmentIndex& other);
    MinimizerHashmapFragmentIndex(MinimizerHashmapFragmentIndex&& other) noexcept;
    MinimizerHashmapFragmentIndex& operator=(MinimizerHashmapFragmentIndex&& other) noexcept;

    static constexpr std::uint32_t kSerializeMagic   = 0xB1000001u;   // unchanged wire format
    static constexpr std::uint32_t kSerializeVersion = 2u;
    static constexpr std::string_view index_name     = MINIMIZERS_PER_FRAGMENT_INDEX_NAME;

    struct SerializedHeader {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint64_t target_len;       ///< v2: full 64-bit target length (corrected from the stale "int32" doc)
        std::uint32_t bucket_count;
        std::uint32_t reserved;         ///< pad; written as 0
        std::uint64_t total_positions;
    };
    struct SerializedBucket {
        std::uint64_t hash;
        std::uint32_t pos_offset;
        std::uint32_t pos_count;
    };
    static constexpr std::size_t kSerializeHeaderBytes = sizeof(SerializedHeader);
    static constexpr std::size_t kSerializeBucketBytes = sizeof(SerializedBucket);
    static_assert(kSerializeHeaderBytes == 32, "wire layout pinned (v2)");
    static_assert(kSerializeBucketBytes == 16, "wire layout pinned (v2)");
    static_assert(std::endian::native == std::endian::little, "wire format requires little-endian host");
    static_assert(sizeof(std::size_t) >= 8, "wire format requires 64-bit size_t");

    [[nodiscard]] std::size_t table_size() const noexcept;
    [[nodiscard]] bool table_empty() const noexcept;
    void for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    template <class F>
    void for_each_table_entry(F&& emit) const {
        using Fn = std::remove_reference_t<F>;
        for_each_table_entry_erased(
            &emit,
            [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
                (*static_cast<Fn*>(ctx))(hash, packed);
            });
    }

    template <class MinimizerT>
    void build(const std::vector<MinimizerT>& target_minimizers,
               sequence_size_t target_length) {
        static const auto* _desc =
            bioseq::instr::register_region_static("target_index_build");
        auto _scope = bioseq::instr::region(_desc);
        target_len = target_length;
        clear_table();
        positions.clear();

        const std::size_t total = target_minimizers.size();
        if (total == 0) {
            return;
        }
        struct KmerKey {
            [[nodiscard, gnu::always_inline]]
            inline std::uint64_t operator()(const MinimizerT& m) const noexcept { return m.kmer(); }
        };
        static thread_local std::vector<MinimizerT> sorted_scratch;
        sorted_scratch.assign(target_minimizers.begin(), target_minimizers.end());
        bioseq::sorting::radix_sort(
            sorted_scratch.data(), sorted_scratch.data() + total, KmerKey{});

        positions.resize(total);
        reserve_table(total / 2 + 16);
        std::size_t i = 0;
        while (i < total) {
            const std::uint64_t kmer = sorted_scratch[i].kmer();
            std::size_t j = i;
            while (j < total && sorted_scratch[j].kmer() == kmer) {
                positions[j] = sorted_scratch[j].pos_strand_encoded();
                ++j;
            }
            emplace_table(kmer,
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
                | static_cast<std::uint64_t>(static_cast<std::uint32_t>(j - i)));
            i = j;
        }
    }

    /// NEW (§9.4): radix-bin over pre-extracted globalised (kmer, encoded_pos) pairs.
    /// `pairs` is MUTABLE and radix-sorted IN PLACE (caller-owned scratch; no copy).
    void build_from_pairs(std::span<std::pair<std::uint64_t,std::uint64_t>> pairs,
                          sequence_size_t target_length);

    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    void add_position_offset(sequence_size_t offset);

    [[nodiscard]] sequence_size_t target_length() const { return target_len; }

    [[nodiscard]] std::size_t serialize_binary_size() const;

    void serialize_to_binary(char* output) const;

    [[nodiscard]] static MinimizerHashmapFragmentIndex deserialize_from_binary(
        const char* input, std::size_t input_size);
};
static_assert(MinimizerIndex<MinimizerHashmapFragmentIndex>,
              "MinimizerHashmapFragmentIndex must model MinimizerIndex");

inline void index_corruption_panic(std::string_view msg) {
    bioseq_log_progress<ERR>("[bioseq index corruption] {}", msg);
    std::abort();
}

/// Caller-owned per-row/per-query assembly scratch (§9.1). Reused across
/// segments within one query, destroyed with its IndexReadContext.
struct MinimizerAssemblyScratch {
    std::vector<std::pair<std::uint64_t,std::uint64_t>> pairs;
    std::vector<std::uint64_t> query_kmers;
    std::vector<std::uint64_t> bucket_positions;
    std::vector<bioseq::indexing::index_sub_key_t> wanted_sub_keys;
    void clear_segment() noexcept { pairs.clear(); }
    void clear_query() noexcept { pairs.clear(); query_kmers.clear(); bucket_positions.clear(); wanted_sub_keys.clear(); }
};

// ---- shared wire walkers (§9.4) ------------------------------------------

/// Walk one chunk's wire blob (existing v2 layout); emit(kmer, encoded_pos).
template <class F>   // F: (uint64 kmer, uint64 encoded_pos) -> void
void wire_for_each_entry(const char* blob, std::size_t blob_bytes, F&& emit) {
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
    if (blob_bytes < sizeof(Header)) index_corruption_panic("index chunk: truncated header");
    Header header{}; std::memcpy(&header, blob, sizeof(Header));
    if (header.magic != MinimizerHashmapFragmentIndex::kSerializeMagic ||
        header.version != MinimizerHashmapFragmentIndex::kSerializeVersion) index_corruption_panic("index chunk: magic/version");
    const std::size_t expect = sizeof(Header) + sizeof(BucketRecord)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(Header);
    const char* pos  = recs + sizeof(BucketRecord) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        BucketRecord bucket{}; std::memcpy(&bucket, recs + sizeof(BucketRecord) * bucket_no, sizeof(BucketRecord));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        for (std::uint32_t pos_no = 0; pos_no < bucket.pos_count; ++pos_no) {
            std::uint64_t encoded{};
            std::memcpy(&encoded, pos + sizeof(std::uint64_t) * (bucket.pos_offset + pos_no), sizeof(encoded));
            emit(bucket.hash, encoded);
        }
    }
}

/// PER-KMER sibling of wire_for_each_entry: cb(kmer, pos_count, gpos).
template <class F>   // F: (uint64 kmer, uint32 pos_count, const uint64* gpos) -> void
void wire_for_each_bucket(const char* blob, std::size_t blob_bytes,
                          std::vector<std::uint64_t>& pos_scratch, F&& cb) {
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
    if (blob_bytes < sizeof(Header)) index_corruption_panic("index chunk: truncated header");
    Header header{}; std::memcpy(&header, blob, sizeof(Header));
    if (header.magic != MinimizerHashmapFragmentIndex::kSerializeMagic ||
        header.version != MinimizerHashmapFragmentIndex::kSerializeVersion) index_corruption_panic("index chunk: magic/version");
    const std::size_t expect = sizeof(Header) + sizeof(BucketRecord)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(Header);
    const char* pos  = recs + sizeof(BucketRecord) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        BucketRecord bucket{}; std::memcpy(&bucket, recs + sizeof(BucketRecord) * bucket_no, sizeof(BucketRecord));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        pos_scratch.resize(bucket.pos_count);
        if (bucket.pos_count > 0)
            std::memcpy(pos_scratch.data(), pos + sizeof(std::uint64_t) * bucket.pos_offset, sizeof(std::uint64_t) * bucket.pos_count);
        cb(bucket.hash, bucket.pos_count, pos_scratch.data());
    }
}

// DOC0345 Patch F — GPU batched mpf helpers. A GPU window covers as many whole
// fragments as fit both the run buffer and the 28-bit position field (with halo).
[[nodiscard]] inline std::size_t gpu_mpf_fragments_per_batch(
    std::size_t nfrag,
    std::size_t frag_bp,
    std::size_t halo_bp,
    std::size_t run_buffer_bytes) noexcept
{
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(MinimapSketchData::pos_field_mask);
    if (nfrag == 0u || frag_bp == 0u) {
        return 1u;
    }
    constexpr std::size_t gpu_segment_local_size = 128u;
    constexpr std::size_t gpu_segment_max_workgroups = 65535u;
    constexpr std::size_t gpu_segment_bytes_per_bp = 96u;
    const std::size_t by_memory = std::max<std::size_t>(
        1u,
        run_buffer_bytes
            / std::max<std::size_t>(
                1u,
                frag_bp * gpu_segment_bytes_per_bp));
    const std::size_t by_pos =
        halo_bp >= max_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_bp - halo_bp) / frag_bp);
    const std::size_t max_dispatch_bp = gpu_segment_local_size * gpu_segment_max_workgroups;
    const std::size_t by_dispatch =
        halo_bp >= max_dispatch_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_dispatch_bp - halo_bp) / frag_bp);
    return std::min({nfrag, by_memory, by_pos, by_dispatch});
}

// Serialize already (kmer, encoded_pos)-sorted pairs (from the GPU sorted-pair
// seam: fused GPU minimizer emit + host pair sort) into the existing mpf
// per-fragment blob format (header + bucket directory + positions). CPU-only
// persistence -- no minimizer collection or sort HERE.
inline void serialize_sorted_pairs_to_fragment_blob(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted_pairs,
    sequence_size_t target_len,
    std::vector<char>& out_blob)
{
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using Bucket = MinimizerHashmapFragmentIndex::SerializedBucket;

    std::vector<Bucket> buckets;
    std::vector<std::uint64_t> positions;
    buckets.reserve(sorted_pairs.size() / 2u + 1u);
    positions.reserve(sorted_pairs.size());

    std::size_t index = 0;
    while (index < sorted_pairs.size()) {
        const std::uint64_t kmer = sorted_pairs[index].first;
        const std::uint32_t offset =
            static_cast<std::uint32_t>(positions.size());
        std::uint32_t count = 0;
        while (index < sorted_pairs.size() && sorted_pairs[index].first == kmer) {
            positions.push_back(sorted_pairs[index].second);
            ++count;
            ++index;
            if (count == UINT32_MAX) {
                index_corruption_panic(
                    "minimizers_per_fragment GPU blob: kmer count exceeds u32");
            }
        }
        buckets.push_back(Bucket{kmer, offset, count});
    }

    const Header header{
        MinimizerHashmapFragmentIndex::kSerializeMagic,
        MinimizerHashmapFragmentIndex::kSerializeVersion,
        static_cast<std::uint64_t>(target_len),
        static_cast<std::uint32_t>(buckets.size()),
        0u,
        static_cast<std::uint64_t>(positions.size()),
    };
    const std::size_t bytes =
        sizeof(Header)
        + buckets.size() * sizeof(Bucket)
        + positions.size() * sizeof(std::uint64_t);
    out_blob.resize(bytes);
    char* cursor = out_blob.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (!buckets.empty()) {
        std::memcpy(cursor, buckets.data(), buckets.size() * sizeof(Bucket));
        cursor += buckets.size() * sizeof(Bucket);
    }
    if (!positions.empty()) {
        std::memcpy(
            cursor,
            positions.data(),
            positions.size() * sizeof(std::uint64_t));
    }
}

// (2) the storage-bound CHUNKED_PER_FRAGMENT index template
template <class S>
struct MinimizersPerFragmentIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MinimizerHashmapFragmentIndex::index_name;
    using storage_type = S;

    template <class R, class Scanner>
    static void build(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend) {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::size_t w = 10, k = 15;                                  // pinned, == sidecar today
        constexpr std::size_t lpu        = traits::letters_per_unit;
        constexpr std::size_t frag_units = Scanner::fragment_storage_units;    // 1024
        constexpr std::size_t frag_bp    = frag_units * lpu;                   // e.g. 32768 @ dna4/uint64
        constexpr std::size_t W          = w + k;                              // backward context
        static_assert(frag_bp < (std::size_t{1} << 28),
            "fragment-core-local positions must fit the 28-bit MinimapSketchData field");
        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if ((std::size_t)seq.size() != (std::size_t)((total_bp + lpu - 1) / lpu))
            index_corruption_panic("minimizers_per_fragment build: scanner unit-span != ceil(total_bp/lpu) (fragment geometry)");

        const std::size_t size_u = (std::size_t)((total_bp + lpu - 1) / lpu);   // total units

        // DOC0345 Patch F — GPU path: batch whole fragments through the GPU
        // sorted-pair seam (fused GPU minimizer emit + host pair sort), scatter
        // each global position into its fragment, and serialize the existing
        // per-fragment blob format. No CPU minimizer COLLECT. Exits before the
        // scalar per-fragment path below.
        if (backend == MinimizerSketchBackend::VulkanSegment) {
            const std::size_t fragments_per_batch = gpu_mpf_fragments_per_batch(
                nfrag,
                frag_bp,
                2u * W,
                storage.run_buffer_bytes());
            std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> per_fragment;
            std::vector<char> blob;
            per_fragment.resize(fragments_per_batch);

            auto write_fragment_blob =
                [&storage](std::size_t fragment_no, const std::vector<char>& fragment_blob) {
                const std::size_t bytes = fragment_blob.size();
                storage.resize(static_cast<index_sub_key_t>(fragment_no), bytes);
                storage.template write_to_each<char>(
                    static_cast<index_sub_key_t>(fragment_no),
                    [&fragment_blob, bytes](
                        std::size_t chunk_no,
                        char* buffer,
                        std::size_t capacity_units) {
                    DEBUG_ASSERT(
                        chunk_no == 0 && capacity_units >= bytes,
                        "one fragment index fits one chunk");
                    if (bytes != 0u) {
                        std::memcpy(buffer, fragment_blob.data(), bytes);
                    }
                    return chunk_no;
                });
            };

            for (std::size_t batch_lo_frag = 0;
                 batch_lo_frag < nfrag;
                 batch_lo_frag += fragments_per_batch) {
                const std::size_t batch_hi_frag =
                    std::min(nfrag, batch_lo_frag + fragments_per_batch);
                const sequence_size_t core_lo =
                    static_cast<sequence_size_t>(batch_lo_frag) * frag_bp;
                const sequence_size_t core_hi = std::min(
                    static_cast<sequence_size_t>(batch_hi_frag) * frag_bp,
                    total_bp);
                const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
                const sequence_size_t coll_hi = std::min(
                    core_hi + static_cast<sequence_size_t>(W),
                    total_bp);
                const std::size_t lo_unit =
                    static_cast<std::size_t>(coll_lo / lpu);
                const std::size_t hi_unit = std::min(
                    static_cast<std::size_t>((coll_hi + lpu - 1) / lpu),
                    size_u);
                const sequence_size_t emit_base =
                    static_cast<sequence_size_t>(lo_unit) * lpu;

                std::vector<std::pair<std::uint64_t, std::uint64_t>> batch_pairs =
                    bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                        seq,
                        lo_unit,
                        hi_unit,
                        emit_base,
                        core_lo,
                        core_hi,
                        w,
                        k);

                for (auto& fragment_pairs : per_fragment) {
                    fragment_pairs.clear();
                }
                for (const auto& pair : batch_pairs) {
                    const sequence_size_t global_pos =
                        static_cast<sequence_size_t>(pair.second >> 1);
                    const std::size_t fragment_no =
                        static_cast<std::size_t>(global_pos / frag_bp);
                    if (fragment_no < batch_lo_frag || fragment_no >= batch_hi_frag) {
                        continue;
                    }
                    const sequence_size_t fragment_core_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const std::uint64_t local_encoded =
                        (static_cast<std::uint64_t>(global_pos - fragment_core_lo) << 1)
                        | (pair.second & 1ull);
                    per_fragment[fragment_no - batch_lo_frag].emplace_back(
                        pair.first,
                        local_encoded);
                }

                for (std::size_t fragment_no = batch_lo_frag;
                     fragment_no < batch_hi_frag;
                     ++fragment_no) {
                    const sequence_size_t fragment_core_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const sequence_size_t fragment_core_hi = std::min(
                        fragment_core_lo + static_cast<sequence_size_t>(frag_bp),
                        total_bp);
                    std::span<const std::pair<std::uint64_t, std::uint64_t>> fragment_pairs{
                        per_fragment[fragment_no - batch_lo_frag].data(),
                        per_fragment[fragment_no - batch_lo_frag].size()};
                    serialize_sorted_pairs_to_fragment_blob(
                        fragment_pairs,
                        fragment_core_hi - fragment_core_lo,
                        blob);
                    write_fragment_blob(fragment_no, blob);
                }
            }

            const std::int64_t manifest = static_cast<std::int64_t>(nfrag);
            storage.resize(DIRECTORY_SUB_KEY, sizeof(std::int64_t));
            storage.template write_to_each<std::int64_t>(
                DIRECTORY_SUB_KEY,
                [manifest](
                    std::size_t chunk_no,
                    std::int64_t* manifest_buffer,
                    std::size_t) {
                manifest_buffer[0] = manifest;
                return chunk_no;
            });
            return;
        }

        auto build_fragment_blob =
            [&seq, total_bp, size_u](std::size_t fragment_no,
                                     sketch_collector& collector,
                                     std::vector<MinimapSketchData>& core,
                                     std::vector<char>& out_blob) {
            const sequence_size_t core_lo = (sequence_size_t)fragment_no * frag_bp;
            const sequence_size_t core_hi = std::min(core_lo + (sequence_size_t)frag_bp, total_bp);
            const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
            const sequence_size_t coll_hi = std::min(core_hi + (sequence_size_t)W, total_bp);
            collector.clear();
            const std::size_t lo_unit = (std::size_t)(coll_lo / lpu);
            const std::size_t hi_unit = std::min((std::size_t)((coll_hi + lpu - 1) / lpu), size_u);
            DEBUG_ASSERT(lo_unit <= hi_unit && hi_unit <= size_u, "sub-range unit bounds out of range");
            auto begin_iter = seq.cbegin(); begin_iter += (sequence_size_t)lo_unit;
            auto end_iter   = seq.cbegin(); end_iter   += (sequence_size_t)hi_unit;
            const sequence_size_t emit_base = (sequence_size_t)lo_unit * lpu;
            const sequence_size_t collect_bp = coll_hi > emit_base ? coll_hi - emit_base : 0;
            core.clear();
            if (collect_bp >= (sequence_size_t)W) {
                collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(collector, seq, begin_iter, end_iter, w, k);
                for (const MinimapSketchData& minimizer : collector.extract_sketches_data()) {
                    const sequence_size_t global_pos = (sequence_size_t)minimizer.last_pos() + emit_base;
                    if (global_pos < core_lo || global_pos >= core_hi) continue; // belongs to a neighbour fragment's core
                    core.push_back(MinimapSketchData{ minimizer.kmer(), (std::uint64_t)(global_pos - core_lo), minimizer.strand() });
                }
            }
            MinimizerHashmapFragmentIndex frag_idx;
            frag_idx.build(core, core_hi - core_lo);   // fragment-local target_len
            out_blob.resize(frag_idx.serialize_binary_size());
            frag_idx.serialize_to_binary(out_blob.data());
        };

        auto write_fragment_blob =
            [&storage](std::size_t fragment_no, const std::vector<char>& blob) {
            const std::size_t bytes = blob.size();
            storage.resize((index_sub_key_t)fragment_no, bytes);
            storage.template write_to_each<char>((index_sub_key_t)fragment_no,
                [&blob, bytes](std::size_t chunk_no, char* buf, std::size_t cap_units) {
                DEBUG_ASSERT(chunk_no == 0 && cap_units >= bytes, "one fragment index fits one chunk");
                if (bytes > 0) {
                    std::memcpy(buf, blob.data(), bytes);
                }
                return chunk_no;
            });
        };

        auto build_and_write_serial =
            [&build_fragment_blob, &write_fragment_blob](std::size_t fragment_no) {
            sketch_collector collector;
            std::vector<MinimapSketchData> core;
            std::vector<char> blob;
            build_fragment_blob(fragment_no, collector, core, blob);
            write_fragment_blob(fragment_no, blob);
        };

        auto& pool = bioseq::parallelism::GlobalPool::instance();
        const bool may_parallel_fragments =
            bioseq::parallelism::task_parallel(pool.mode())
            && pool.max_concurrency() > 1
            && bioseq::storage::lazy_is_worker_shareable(seq)
            && nfrag >= bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT;

        if (!may_parallel_fragments) {
            for (std::size_t fragment_no = 0; fragment_no < nfrag; ++fragment_no) {
                build_and_write_serial(fragment_no);
            }
        } else {
            const std::uint64_t build_peak =
                bioseq::parallelism::minimizer_index_peak_bytes_for_bp(frag_bp);
            const std::uint64_t peak_per_fragment =
                bioseq::parallelism::saturating_mul(2, build_peak);

            for (std::size_t wave_lo = 0; wave_lo < nfrag;) {
                auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
                    pool.max_concurrency(), peak_per_fragment);
                if (!wave.admitted) {
                    build_and_write_serial(wave_lo);
                    ++wave_lo;
                    continue;
                }

                const std::size_t wave_hi = std::min(
                    nfrag, wave_lo + static_cast<std::size_t>(wave.fanout));
                std::vector<std::vector<char>> blobs(wave_hi - wave_lo);

                bioseq::parallelism::parallel_for_pipeline(
                    0, blobs.size(), 1,
                    bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT,
                    bioseq::parallelism::TaskParallelRegion::fragment_build,
                    [&build_fragment_blob, &blobs, wave_lo](
                            std::size_t lo, std::size_t hi) {
                        sketch_collector collector;
                        std::vector<MinimapSketchData> core;
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            build_fragment_blob(wave_lo + rel, collector, core, blobs[rel]);
                        }
                    });

                for (std::size_t rel = 0; rel < blobs.size(); ++rel) {
                    write_fragment_blob(wave_lo + rel, blobs[rel]);
                }
                wave_lo = wave_hi;
            }
        }
        // COMPLETENESS MANIFEST (written LAST): a tiny chunk at DIRECTORY_SUB_KEY recording nfrag.
        { const std::int64_t manifest = (std::int64_t)nfrag;
          storage.resize(DIRECTORY_SUB_KEY, sizeof(std::int64_t));
          storage.template write_to_each<std::int64_t>(DIRECTORY_SUB_KEY, [manifest](std::size_t chunk_no, std::int64_t* manifest_buf, std::size_t){ manifest_buf[0]=manifest; return chunk_no; }); }
    }

    // DOC0345 Patch F — scalar-default overload delegates to the explicit backend.
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

    /// query-time: materialise the single pipeline IndexT for one target SEGMENT.
    static MinimizerHashmapFragmentIndex assemble_segment(
            S& storage, std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            MinimizerAssemblyScratch& scratch) {
        auto& pairs = scratch.pairs;
        scratch.clear_segment();
        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(DIRECTORY_SUB_KEY) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(DIRECTORY_SUB_KEY, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;      // CLIP to the segment's collection range
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        MinimizerHashmapFragmentIndex merged;
        merged.build_from_pairs(pairs, total_bp);   // FULL target length
        return merged;
    }

    /// Query-filtered segment assembly for GPU seed lookup. Only query kmers
    /// can be probed by the seed stage, so persisted MPF does not need to
    /// materialise unrelated buckets before uploading the segment index.
    static MinimizerHashmapFragmentIndex assemble_segment_for_query(
            S& storage,
            std::span<const MinimapSketchData> query_minimizers,
            std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            MinimizerAssemblyScratch& scratch) {
        scratch.clear_query();
        auto& pairs = scratch.pairs;
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        for (const MinimapSketchData& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
        }
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());

        clip_hi_bp = std::min(clip_hi_bp, total_bp);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }

        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(DIRECTORY_SUB_KEY) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(DIRECTORY_SUB_KEY, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer)) return;
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        MinimizerHashmapFragmentIndex merged;
        merged.build_from_pairs(pairs, total_bp);
        return merged;
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<MinimizersPerFragmentIndex> {
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MinimizerHashmapFragmentIndex::index_name;
    static constexpr bool allows_update_on_insert = true;   // cheap per-fragment build; DEFAULT on-insert
};
static_assert(requires { index_descriptor<MinimizersPerFragmentIndex>::index_name; },
              "MinimizersPerFragmentIndex index_descriptor must be exported");
}
