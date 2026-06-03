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
#include <span>
#include <string_view>
#include <utility>
#include <vector>
export module bioseq_indexing.minimizer_sequence;
import bioseq_indexing.type;
import bioseq_indexing.storage;
import bioseq_indexing.traits;
import bioseq_indexing.minimizer_hashmap;            // MinimizerHashmapFragmentIndex, build_from_pairs, wire_for_each_*
import bioseq_sequences.kmers;                       // collect_minimizers_view, sketch_collector, MinimapSketchData
import bioseq_sequences.kmers.hashers.minimap_int;   // minimap_int_hasher
import bioseq_storage_lazy;                          // lazy_compact_sequence
import bioseq_storage_size;                          // sequence_size_t
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers_view;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
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
}  // namespace detail

template <class S>
struct MinimizerSequenceMergedIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
    static_assert(is_external_sort_spill_storage<S>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = "minimizers_per_sequence";
    using storage_type = S;

    template <class R, class Scanner>
    static void build(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
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
        std::vector<std::pair<std::uint64_t,std::uint64_t>> buf;
        std::size_t buf_bytes = 0;
        sketch_collector collector;
        const std::size_t size_u = (std::size_t)((total_bp + lpu - 1) / lpu);
        for (std::size_t fragment_no = 0; fragment_no < nfrag; ++fragment_no) {
            const sequence_size_t core_lo = (sequence_size_t)fragment_no * frag_bp;
            const sequence_size_t core_hi = std::min(core_lo + (sequence_size_t)frag_bp, total_bp);
            const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
            const sequence_size_t coll_hi = std::min(core_hi + (sequence_size_t)W, total_bp);
            collector.clear();
            const std::size_t lo_unit = (std::size_t)(coll_lo / lpu);
            const std::size_t hi_unit = std::min((std::size_t)((coll_hi + lpu - 1) / lpu), size_u);
            auto begin_iter = seq.cbegin(); begin_iter += (sequence_size_t)lo_unit;
            auto end_iter   = seq.cbegin(); end_iter   += (sequence_size_t)hi_unit;
            const sequence_size_t emit_base = (sequence_size_t)lo_unit * lpu;
            const sequence_size_t collect_bp = coll_hi > emit_base ? coll_hi - emit_base : 0;
            if (collect_bp >= (sequence_size_t)W) {
                collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(collector, seq, begin_iter, end_iter, w, k);
                for (const MinimapSketchData& m : collector.extract_sketches_data()) {
                    const sequence_size_t gpos = (sequence_size_t)m.last_pos() + emit_base;
                    if (gpos < core_lo || gpos >= core_hi) continue;
                    buf.emplace_back(m.kmer(), ((std::uint64_t)gpos << 1) | (std::uint64_t)m.strand());
                    buf_bytes += sizeof(std::pair<std::uint64_t,std::uint64_t>);
                }
            }
            if (buf_bytes >= run_buffer_bytes && !buf.empty()) {
                std::sort(buf.begin(), buf.end());                              // composite (kmer, encoded)
                storage.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>(buf));
                buf.clear(); buf_bytes = 0;
            }
        }
        if (!buf.empty()) {
            std::sort(buf.begin(), buf.end());
            storage.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>(buf));
            buf.clear(); buf_bytes = 0;
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
        storage.release_runs();
    }

    template <class R, class Scanner>
    static void rebuild(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
        storage.clear_entity(); build(storage, seq);
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
    static constexpr std::string_view  index_name = "minimizers_per_sequence";
    static constexpr bool allows_update_on_insert = false;  // whole-sequence external sort — UPDATE_MANUALLY only
};
static_assert(requires { index_descriptor<MinimizerSequenceMergedIndex>::index_name; },
              "MinimizerSequenceMergedIndex index_descriptor must be exported");
}
